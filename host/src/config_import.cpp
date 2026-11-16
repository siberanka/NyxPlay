/**
 * @file src/config_import.cpp
 * @brief Definitions for importing the configuration of another host (Apollo / Sunshine).
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "config_import.h"
#include "crypto.h"
#include "uuid.h"

namespace fs = std::filesystem;
using namespace std::literals;

namespace config_import {
  namespace {
    using json = nlohmann::ordered_json;

    // Location and instance specific keys that must not be carried over.
    constexpr std::array<std::string_view, 6> skipped_keys {"file_apps", "file_state", "credentials_file", "pkey", "cert", "log_path"};

    struct entry_t {
      std::string key;
      std::string value;
    };

    struct locations_t {
      fs::path conf;
      fs::path apps;
      fs::path state;
      fs::path credentials;
      fs::path pkey;
      fs::path cert;
    };

    std::string to_u8(const fs::path &p) {
      auto s = p.u8string();
      return std::string(s.begin(), s.end());
    }

    fs::path from_u8(const std::string &s) {
      return fs::path(std::u8string(s.begin(), s.end()));
    }

    fs::path get_env(const char *name [[maybe_unused]], const wchar_t *wname [[maybe_unused]]) {
#ifdef _WIN32
      // Environment values may hold non-ASCII characters, so use the wide API.
      const wchar_t *value = _wgetenv(wname);
      return value && *value ? fs::path(std::wstring(value)) : fs::path();
#else
      const char *value = std::getenv(name);
      return value && *value ? fs::path(value) : fs::path();
#endif
    }

    bool read_file(const fs::path &path, std::string &out) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return false;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      if (in.bad()) {
        return false;
      }
      out = ss.str();
      return true;
    }

    bool is_file(const fs::path &p) {
      std::error_code ec;
      return fs::is_regular_file(p, ec);
    }

    // The key and certificate paths come from the other host's configuration, so only a parsable PEM
    // certificate with its matching private key is taken over as the host identity
    bool is_identity_pair(const fs::path &key_path, const fs::path &cert_path) {
      constexpr std::uintmax_t max_pem_size = 64 * 1024;
      std::error_code ec;
      if (fs::file_size(key_path, ec) > max_pem_size || ec || fs::file_size(cert_path, ec) > max_pem_size || ec) {
        return false;
      }

      std::string key_text;
      std::string cert_text;
      if (!read_file(key_path, key_text) || !read_file(cert_path, cert_text)) {
        return false;
      }

      auto key = crypto::pkey(key_text);
      auto cert = crypto::x509(cert_text);
      return key && cert && X509_check_private_key(cert.get(), key.get()) == 1;
    }

    bool is_dir(const fs::path &p) {
      std::error_code ec;
      return fs::is_directory(p, ec);
    }

    /**
     * @brief Lower-case ASCII only, so byte offsets stay the same as in the original string.
     */
    std::string fold(std::string s, bool case_insensitive) {
      for (auto &c : s) {
        if (c == '\\') {
          c = '/';
        } else if (case_insensitive && c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c - 'A' + 'a');
        }
      }
      return s;
    }

#ifdef _WIN32
    constexpr bool case_insensitive_fs = true;
#else
    constexpr bool case_insensitive_fs = false;
#endif

    std::string normalized_dir(const fs::path &dir) {
      auto s = fold(to_u8(dir.lexically_normal()), case_insensitive_fs);
      while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
      }
      return s;
    }

    /**
     * @brief Return the part of @p text below @p dir_norm (a normalized directory), if any.
     */
    std::optional<std::string> relative_below(const std::string &text, const std::string &dir_norm) {
      if (dir_norm.empty() || dir_norm == "/") {
        return std::nullopt;
      }
      auto folded = fold(text, case_insensitive_fs);
      if (folded.size() <= dir_norm.size() + 1 || folded.compare(0, dir_norm.size(), dir_norm) != 0 || folded[dir_norm.size()] != '/') {
        return std::nullopt;
      }
      std::string rel = text.substr(dir_norm.size() + 1);
      std::replace(rel.begin(), rel.end(), '\\', '/');
      return rel;
    }

    bool contains_dir(const std::string &text, const std::string &dir_norm) {
      if (dir_norm.empty() || dir_norm == "/") {
        return false;
      }
      auto folded = fold(text, case_insensitive_fs);
      for (auto pos = folded.find(dir_norm); pos != std::string::npos; pos = folded.find(dir_norm, pos + 1)) {
        auto after = pos + dir_norm.size();
        if (after == folded.size() || folded[after] == '/' || folded[after] == '"' || folded[after] == '\'' || folded[after] == ' ') {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief A relative path coming from an untrusted file must stay inside the directory it is joined to.
     */
    bool safe_relative(const std::string &rel) {
      if (rel.empty()) {
        return false;
      }
      fs::path p = from_u8(rel);
      if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        return false;
      }
      return std::none_of(p.begin(), p.end(), [](const fs::path &part) {
        return part == "..";
      });
    }

    // ---- sunshine.conf parsing, mirrors config.cpp (parse_option / parse_config) ----

    bool is_endline(char c) {
      return c == '\n' || c == '\r';
    }

    bool is_space_tab(char c) {
      return c == ' ' || c == '\t';
    }

    bool is_whitespace(char c) {
      return is_space_tab(c) || is_endline(c);
    }

    // Drop '#' comments up to the end of each line, keep line breaks.
    std::string strip_comments(std::string_view s) {
      std::string result;
      size_t pos = 0;
      while (pos < s.size()) {
        auto comment = s.find('#', pos);
        if (comment == std::string_view::npos) {
          result.append(s.substr(pos));
          break;
        }
        result.append(s.substr(pos, comment - pos));
        pos = comment;
        while (pos < s.size() && !is_endline(s[pos])) {
          ++pos;
        }
      }
      return result;
    }

    std::vector<entry_t> parse_conf(std::string_view s) {
      std::vector<entry_t> entries;
      const size_t n = s.size();
      size_t pos = 0;

      while (pos < n) {
        size_t begin = pos;
        while (begin < n && is_whitespace(s[begin])) {
          ++begin;
        }
        size_t endl = begin;
        while (endl < n && !is_endline(s[endl])) {
          ++endl;
        }
        size_t endc = begin;
        while (endc < endl && s[endc] != '#') {
          ++endc;
        }
        while (endc > begin && is_whitespace(s[endc - 1])) {
          --endc;
        }
        size_t eq = begin;
        while (eq < endc && s[eq] != '=') {
          ++eq;
        }

        if (eq != endc && eq != begin) {
          size_t end_name = eq;
          while (end_name > begin && is_space_tab(s[end_name - 1])) {
            --end_name;
          }
          size_t begin_val = eq + 1;
          while (begin_val < endc && is_space_tab(s[begin_val])) {
            ++begin_val;
          }

          if (begin_val != endl) {
            // Lists may span several lines, skip to the matching bracket like parse_config does.
            if (s[begin_val] == '[') {
              int depth = 1;
              size_t i = begin_val + 1;
              while (i < n && depth) {
                if (s[i] == '[') {
                  ++depth;
                } else if (s[i] == ']') {
                  --depth;
                }
                ++i;
              }
              endl = i;
            }

            entry_t entry;
            entry.key = strip_comments(s.substr(begin, end_name - begin));
            entry.value = strip_comments(s.substr(begin_val, endl - begin_val));
            while (!entry.value.empty() && is_whitespace(entry.value.back())) {
              entry.value.pop_back();
            }
            // The first occurrence of a key wins, like the emplace in parse_config.
            bool seen = std::any_of(entries.begin(), entries.end(), [&](const entry_t &e) {
              return e.key == entry.key;
            });
            if (!entry.key.empty() && !entry.value.empty() && !seen) {
              entries.push_back(std::move(entry));
            }
          }
        }

        pos = endl;
        if (pos < n) {
          pos += (s[pos] == '\r') ? 2 : 1;
        }
      }

      return entries;
    }

    const entry_t *find_entry(const std::vector<entry_t> &entries, std::string_view key) {
      for (auto &e : entries) {
        if (e.key == key) {
          return &e;
        }
      }
      return nullptr;
    }

    // ---- locations ----

    fs::path resolve_path(const fs::path &dir, const std::vector<entry_t> &entries, std::string_view key, const fs::path &fallback) {
      fs::path p = fallback;
      if (auto e = find_entry(entries, key)) {
        p = from_u8(e->value);
      }
      return p.is_relative() ? dir / p : p;
    }

    locations_t resolve_locations(const fs::path &dir, const std::vector<entry_t> &entries) {
      locations_t loc;
      loc.conf = dir / "sunshine.conf";
      loc.apps = resolve_path(dir, entries, "file_apps", "apps.json");
      loc.state = resolve_path(dir, entries, "file_state", "sunshine_state.json");
      // The credentials file defaults to the state file, as in apply_config.
      loc.credentials = resolve_path(dir, entries, "credentials_file", loc.state);
      loc.pkey = resolve_path(dir, entries, "pkey", fs::path("credentials") / "cakey.pem");
      loc.cert = resolve_path(dir, entries, "cert", fs::path("credentials") / "cacert.pem");
      return loc;
    }

    std::vector<entry_t> load_conf(const fs::path &dir) {
      std::string content;
      if (read_file(dir / "sunshine.conf", content)) {
        // A byte order mark would become part of the first key
        if (content.starts_with("\xEF\xBB\xBF")) {
          content.erase(0, 3);
        }
        return parse_conf(content);
      }
      return {};
    }

    std::string guess_product(const fs::path &dir) {
      for (auto p = dir.lexically_normal(); !p.empty() && p.has_filename(); p = p.parent_path()) {
        auto name = fold(to_u8(p.filename()), true);
        if (name == "apollo") {
          return "Apollo";
        }
        if (name == "sunshine") {
          return "Sunshine";
        }
        if (p == p.parent_path()) {
          break;
        }
      }
      return "Custom";
    }

    // ---- JSON helpers ----

    bool parse_json(const std::string &text, json &out) {
      std::string_view view = text;
      if (view.substr(0, 3) == "\xEF\xBB\xBF"sv) {
        view.remove_prefix(3);
      }
      try {
        out = json::parse(view.begin(), view.end());
        return true;
      } catch (const std::exception &) {
        return false;
      }
    }

    bool load_json(const fs::path &path, json &out) {
      std::string text;
      return read_file(path, text) && parse_json(text, out);
    }

    // ---- file operations ----

    std::string timestamp() {
      auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm {};
#ifdef _WIN32
      localtime_s(&tm, &t);
#else
      localtime_r(&t, &tm);
#endif
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
      return buf;
    }

    // Copies files that are about to be replaced into import-backup-<timestamp>/ inside the target.
    class backup_t {
    public:
      explicit backup_t(fs::path target):
          target_(std::move(target)) {}

      const fs::path &dir() const {
        return dir_;
      }

      bool save(const fs::path &file, std::string &error) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
          return true;
        }
        if (dir_.empty()) {
          auto base = "import-backup-" + timestamp();
          auto candidate = target_ / base;
          for (int i = 2; fs::exists(candidate, ec); ++i) {
            candidate = target_ / (base + "-" + std::to_string(i));
          }
          dir_ = candidate;
        }
        auto rel = file.lexically_relative(target_);
        if (rel.empty() || *rel.begin() == "..") {
          rel = file.filename();
        }
        auto dest = dir_ / rel;
        fs::create_directories(dest.parent_path(), ec);
        if (ec) {
          error = "cannot create backup directory " + to_u8(dest.parent_path()) + ": " + ec.message();
          return false;
        }
        fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
          error = "cannot back up " + to_u8(file) + ": " + ec.message();
          return false;
        }
        return true;
      }

    private:
      fs::path target_;
      fs::path dir_;
    };

    bool replace_file(const fs::path &tmp, const fs::path &dest, std::string &error) {
      std::error_code ec;
      fs::rename(tmp, dest, ec);
      if (ec) {
        // Older platforms refuse to rename over an existing file.
        fs::remove(dest, ec);
        ec.clear();
        fs::rename(tmp, dest, ec);
      }
      if (ec) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        error = "cannot write " + to_u8(dest) + ": " + ec.message();
        return false;
      }
      return true;
    }

    bool write_atomic(const fs::path &dest, const std::string &content, backup_t &backup, std::string &error, bool restrict_permissions = false) {
      std::error_code ec;
      fs::create_directories(dest.parent_path(), ec);
      if (ec) {
        error = "cannot create directory " + to_u8(dest.parent_path()) + ": " + ec.message();
        return false;
      }
      if (!backup.save(dest, error)) {
        return false;
      }
      auto tmp = dest;
      tmp += ".tmp";
      if (restrict_permissions) {
        // Key material: limit access before the first byte is written
        {
          std::ofstream create(tmp, std::ios::binary | std::ios::trunc);
        }
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
      }
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
          std::error_code ignore;
          fs::remove(tmp, ignore);
          error = "cannot write " + to_u8(dest);
          return false;
        }
      }
      return replace_file(tmp, dest, error);
    }

    /**
     * @brief Copy a file byte for byte and verify the result. Used for the private key as well.
     */
    bool copy_atomic(const fs::path &from, const fs::path &dest, bool restrict_permissions, backup_t &backup, std::string &error) {
      std::string data;
      if (!read_file(from, data)) {
        error = "cannot read " + to_u8(from);
        return false;
      }
      if (!write_atomic(dest, data, backup, error, restrict_permissions)) {
        return false;
      }
      std::string check;
      if (!read_file(dest, check) || check != data) {
        error = "verification of the copy failed for " + to_u8(dest);
        return false;
      }
      if (restrict_permissions) {
        std::error_code ec;
        fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
      }
      return true;
    }

    bool same_directory(const fs::path &a, const fs::path &b) {
      std::error_code ec;
      if (fs::exists(a, ec) && fs::exists(b, ec)) {
        std::error_code ec2;
        if (fs::equivalent(a, b, ec2) && !ec2) {
          return true;
        }
      }
      auto ca = fs::weakly_canonical(a, ec);
      auto cb = fs::weakly_canonical(b, ec);
      return normalized_dir(ca) == normalized_dir(cb);
    }

    // ---- staged work ----

    struct staged_text_t {
      fs::path dest;
      std::string content;
    };

    struct staged_copy_t {
      fs::path from;
      fs::path dest;
      bool secret = false;
    };

    /**
     * @brief What normalize_imported_clients had to change in an imported client list.
     */
    struct client_normalization_t {
      size_t defaulted = 0;  ///< Clients that got the default permission (no usable permission was given).
      size_t dropped = 0;  ///< Entries without a usable certificate and repeated certificates.
      size_t renumbered = 0;  ///< Clients that got a new identifier (empty or repeated).
      size_t elevated = 0;  ///< Clients that keep file, command or clipboard permissions.
    };

    /**
     * @brief Make an imported client list safe to load.
     * @details Devices without a usable permission get the default permission set (they would
     *          otherwise be loaded with full access) and keep client commands disabled. Unknown
     *          permission bits are removed. Entries without a certificate and repeated certificates
     *          are dropped, repeated or empty identifiers are replaced. Devices that carry a valid
     *          permission keep it.
     */
    client_normalization_t normalize_imported_clients(json &troot) {
      client_normalization_t stats;
      if (!troot.contains("named_devices") || !troot["named_devices"].is_array()) {
        troot["named_devices"] = json::array();
      }

      // The legacy layout has no per device settings and is loaded with full access: convert it
      if (auto devices = troot.find("devices"); devices != troot.end()) {
        if (devices->is_array()) {
          for (auto &device : *devices) {
            auto certs = device.is_object() ? device.find("certs") : device.end();
            if (device.is_object() && certs != device.end() && certs->is_array()) {
              for (auto &cert : *certs) {
                if (cert.is_string()) {
                  troot["named_devices"].push_back({{"name", ""}, {"cert", cert}, {"uuid", uuid_util::uuid_t::generate().string()}, {"perm", static_cast<uint32_t>(crypto::PERM::_default)}, {"allow_client_commands", false}});
                  ++stats.defaulted;
                }
              }
            }
          }
        }
        troot.erase("devices");
      }

      // Taken after the erase above, which moves the remaining members of the object
      auto &named = troot["named_devices"];

      constexpr auto elevated_bits = static_cast<uint32_t>(crypto::PERM::file_upload) | static_cast<uint32_t>(crypto::PERM::file_dwnload) | static_cast<uint32_t>(crypto::PERM::server_cmd) | static_cast<uint32_t>(crypto::PERM::clipboard_set) | static_cast<uint32_t>(crypto::PERM::clipboard_read);
      std::set<std::string> seen_certs;
      std::set<std::string> seen_uuids;
      json cleaned = json::array();
      for (auto &el : named) {
        if (!el.is_object() || !el.contains("cert") || !el["cert"].is_string() || el["cert"].get<std::string>().empty() ||
            !seen_certs.insert(el["cert"].get<std::string>()).second) {
          ++stats.dropped;
          continue;
        }

        bool perm_usable = false;
        if (el.contains("perm")) {
          const auto &perm = el["perm"];
          if (perm.is_number_unsigned() || (perm.is_number_integer() && perm.get<int64_t>() >= 0)) {
            const auto raw = perm.get<uint64_t>();
            if (raw <= 0xFFFFFFFFull) {
              const auto masked = static_cast<uint32_t>(raw) & static_cast<uint32_t>(crypto::PERM::_all);
              el["perm"] = masked;
              perm_usable = true;
              if ((masked & elevated_bits) != 0) {
                ++stats.elevated;
              }
            }
          }
        }
        if (!perm_usable) {
          el["perm"] = static_cast<uint32_t>(crypto::PERM::_default);
          if (!el.contains("allow_client_commands")) {
            el["allow_client_commands"] = false;
          }
          ++stats.defaulted;
        }

        // Every device needs a unique identifier to be told apart from the others
        if (!el.contains("uuid") || !el["uuid"].is_string() || el["uuid"].get<std::string>().empty() ||
            !seen_uuids.insert(el["uuid"].get<std::string>()).second) {
          el["uuid"] = uuid_util::uuid_t::generate().string();
          seen_uuids.insert(el["uuid"].get<std::string>());
          ++stats.renumbered;
        }
        cleaned.push_back(std::move(el));
      }
      named = std::move(cleaned);
      return stats;
    }

    /**
     * @brief Number of paired devices described by a state "root" node (both layouts).
     */
    size_t count_clients(const json &root) {
      size_t count = 0;
      if (auto it = root.find("named_devices"); it != root.end() && it->is_array()) {
        count += it->size();
      }
      if (auto it = root.find("devices"); it != root.end() && it->is_array()) {
        for (auto &device : *it) {
          if (device.is_object()) {
            if (auto certs = device.find("certs"); certs != device.end() && certs->is_array()) {
              count += certs->size();
            }
          }
        }
      }
      return count;
    }

    bool run_import(const fs::path &source, const fs::path &target, const options_t &options, result_t &result) {
      std::error_code ec;

      if (!is_dir(source)) {
        result.error = "source directory does not exist: " + to_u8(source);
        return false;
      }
      if (same_directory(source, target)) {
        result.error = "source and target are the same directory";
        return false;
      }

      auto src_entries = load_conf(source);
      auto src_loc = resolve_locations(source, src_entries);
      const bool have_conf = is_file(src_loc.conf);
      const bool have_apps = is_file(src_loc.apps);
      const bool have_state = is_file(src_loc.state);
      const bool have_key = is_file(src_loc.pkey);
      const bool have_cert = is_file(src_loc.cert);

      if (!have_conf && !have_apps && !have_state && !(have_key && have_cert)) {
        result.error = "no importable files found in " + to_u8(source);
        return false;
      }

      const auto source_norm = normalized_dir(source);
      const auto install_norm = normalized_dir(source.lexically_normal().parent_path());
      const auto assets_norm = install_norm.empty() ? std::string() : install_norm + "/assets";

      std::vector<staged_text_t> texts;
      std::vector<staged_copy_t> copies;
      std::vector<std::string> imported;
      std::map<std::string, json> target_docs;  // Parsed target JSON files, keyed by path.
      std::map<std::string, fs::path> target_doc_paths;
      std::vector<entry_t> target_entries;  // Settings the target will have after the import.

      // Effective settings of the target after this import decide where its other files live.
      if (options.settings && have_conf) {
        // Path keys are dropped, so the target falls back to its defaults.
        target_entries = {};
      } else {
        target_entries = load_conf(target);
      }
      auto tgt_loc = resolve_locations(target, target_entries);

      auto target_doc = [&](const fs::path &path) -> json * {
        auto key = to_u8(path.lexically_normal());
        auto it = target_docs.find(key);
        if (it != target_docs.end()) {
          return &it->second;
        }
        json doc = json::object();
        if (is_file(path) && (!load_json(path, doc) || !doc.is_object())) {
          result.error = "existing target file is not valid JSON: " + to_u8(path);
          return nullptr;
        }
        target_doc_paths[key] = path;
        return &(target_docs[key] = std::move(doc));
      };

      // ---- settings ----
      if (options.settings) {
        if (!have_conf) {
          result.warnings.push_back("settings: sunshine.conf not found, skipped");
        } else {
          std::string out;
          size_t count = 0;
          for (auto &e : src_entries) {
            if (std::find(skipped_keys.begin(), skipped_keys.end(), e.key) != skipped_keys.end()) {
              continue;
            }
            if (e.key == "port") {
              if (!options.keep_port) {
                continue;
              }
              result.warnings.push_back("settings: port " + e.value + " is kept; the other host must be stopped or use another port");
            }
            if (!config::is_known_option(e.key)) {
              result.warnings.push_back("settings: \"" + e.key + "\" is not a known setting of this host and has no effect");
            }
            if (contains_dir(e.value, install_norm)) {
              result.warnings.push_back("settings: \"" + e.key + "\" refers to the other installation directory, review it");
            }
            out += e.key + " = " + e.value + "\n";
            ++count;
          }
          texts.push_back({tgt_loc.conf, std::move(out)});
          imported.push_back("settings (" + std::to_string(count) + " options)");
        }
      }

      // ---- applications ----
      if (options.apps) {
        if (!have_apps) {
          result.warnings.push_back("applications: apps file not found, skipped");
        } else {
          json apps;
          if (!load_json(src_loc.apps, apps) || !apps.is_object()) {
            result.error = "applications file is not valid JSON: " + to_u8(src_loc.apps);
            return false;
          }

          size_t app_count = 0;
          std::map<std::string, bool> planned;  // Destination (normalized) -> already planned.
          auto plan_copy = [&](const fs::path &from, const fs::path &dest) {
            if (planned.emplace(to_u8(dest.lexically_normal()), true).second) {
              copies.push_back({from, dest, false});
            }
          };

          if (auto it = apps.find("apps"); it != apps.end() && it->is_array()) {
            for (auto &app : *it) {
              if (!app.is_object()) {
                continue;
              }
              ++app_count;
              auto img = app.find("image-path");
              if (img == app.end() || !img->is_string()) {
                continue;
              }
              const std::string path = img->get<std::string>();
              if (auto rel = relative_below(path, assets_norm); rel && rel->find('/') == std::string::npos) {
                // The host resolves bare names in its own assets directory.
                *img = *rel;
              } else if (auto crel = relative_below(path, source_norm); crel && safe_relative(*crel)) {
                auto from = source / from_u8(*crel);
                if (is_file(from)) {
                  auto dest = target / from_u8(*crel);
                  plan_copy(from, dest);
                  *img = to_u8(fs::path(dest).make_preferred());
                } else {
                  result.warnings.push_back("applications: cover not found: " + path);
                }
              }
            }
          } else {
            result.warnings.push_back("applications: the file has no \"apps\" list");
          }

          texts.push_back({tgt_loc.apps, apps.dump(4)});
          imported.push_back("applications (" + std::to_string(app_count) + ")");

          // Covers that no app references yet still belong to the library.
          auto covers = source / "covers";
          size_t cover_files = 0;
          if (is_dir(covers)) {
            for (fs::recursive_directory_iterator it(covers, ec), end; !ec && it != end; it.increment(ec)) {
              std::error_code fec;
              if (it->is_regular_file(fec)) {
                plan_copy(it->path(), target / it->path().lexically_relative(source));
              }
            }
          }
          cover_files = copies.size();
          if (cover_files) {
            imported.push_back("covers (" + std::to_string(cover_files) + " files)");
          }
        }
      }

      // ---- paired clients and host identity ----
      if (options.clients) {
        json src_state;
        bool state_ok = false;
        if (!have_state) {
          result.warnings.push_back("clients: state file not found, no paired clients imported");
        } else if (!load_json(src_loc.state, src_state) || !src_state.is_object()) {
          result.error = "state file is not valid JSON: " + to_u8(src_loc.state);
          return false;
        } else {
          state_ok = true;
        }

        const json *root = nullptr;
        if (state_ok) {
          if (auto it = src_state.find("root"); it != src_state.end() && it->is_object() && it->contains("uniqueid") && (*it)["uniqueid"].is_string()) {
            root = &*it;
          } else {
            result.warnings.push_back("clients: state file has no root.uniqueid, paired clients skipped");
          }
        }

        const size_t clients = root ? count_clients(*root) : 0;
        const bool identity = have_key && have_cert && is_identity_pair(src_loc.pkey, src_loc.cert);
        bool take_state = root != nullptr;
        bool take_identity = identity;

        if (have_key && have_cert && !identity) {
          result.warnings.push_back("clients: host certificate and private key are not a valid matching PEM pair, identity skipped");
        }

        if (clients > 0 && !identity) {
          // Paired clients pin the host certificate, so they are useless without it.
          result.warnings.push_back("clients: host certificate or private key is missing, paired clients skipped");
          take_state = false;
          take_identity = false;
        } else if (!identity && (have_key || have_cert)) {
          result.warnings.push_back("clients: only one of host certificate and private key exists, identity skipped");
        }

        if (take_state) {
          auto doc = target_doc(tgt_loc.state);
          if (!doc) {
            return false;
          }
          size_t replaced = 0;
          if (auto tr = doc->find("root"); tr != doc->end() && tr->is_object()) {
            replaced = count_clients(*tr);
          }
          if (!doc->contains("root") || !(*doc)["root"].is_object()) {
            (*doc)["root"] = json::object();
          }
          auto &troot = (*doc)["root"];
          troot["uniqueid"] = (*root)["uniqueid"];
          // Both layouts are accepted by load_state, mirror what the source has.
          troot.erase("named_devices");
          troot.erase("devices");
          if (root->contains("named_devices")) {
            troot["named_devices"] = (*root)["named_devices"];
          }
          if (root->contains("devices")) {
            troot["devices"] = (*root)["devices"];
          }
          const auto normalized = normalize_imported_clients(troot);
          if (normalized.defaulted > 0) {
            result.warnings.push_back("clients: " + std::to_string(normalized.defaulted) + " imported client(s) were given the default permission; adjust them in the Web UI");
          }
          if (normalized.dropped > 0) {
            result.warnings.push_back("clients: " + std::to_string(normalized.dropped) + " entr(ies) without a usable certificate or with a repeated certificate were skipped");
          }
          if (normalized.renumbered > 0) {
            result.warnings.push_back("clients: " + std::to_string(normalized.renumbered) + " client(s) had a missing or repeated identifier and got a new one");
          }
          if (normalized.elevated > 0) {
            result.warnings.push_back("clients: " + std::to_string(normalized.elevated) + " imported client(s) may transfer files, run host commands or use the clipboard; review them in the Web UI");
          }
          if (replaced > 0) {
            result.warnings.push_back("clients: " + std::to_string(replaced) + " paired client(s) of this host were replaced (see backup)");
          }
          imported.push_back("paired clients (" + std::to_string(clients) + ")");
        } else if (take_identity && !root) {
          // Pairings of this host pin its current certificate; do not swap it without the matching client list.
          auto doc = target_doc(tgt_loc.state);
          if (!doc) {
            return false;
          }
          if (auto tr = doc->find("root"); tr != doc->end() && tr->is_object() && count_clients(*tr) > 0) {
            result.warnings.push_back("clients: host identity not imported, this host has paired clients that depend on its certificate");
            take_identity = false;
          }
        }

        if (take_identity) {
          copies.push_back({src_loc.pkey, tgt_loc.pkey, true});
          copies.push_back({src_loc.cert, tgt_loc.cert, false});
          imported.push_back("host identity (certificate and private key)");
        }
      }

      // ---- web credentials ----
      if (options.web_credentials) {
        json creds;
        if (!is_file(src_loc.credentials)) {
          result.warnings.push_back("web credentials: credentials file not found, skipped");
        } else if (!load_json(src_loc.credentials, creds) || !creds.is_object()) {
          result.error = "credentials file is not valid JSON: " + to_u8(src_loc.credentials);
          return false;
        } else if (!creds.contains("username") || !creds.contains("salt") || !creds.contains("password") ||
                   !creds["username"].is_string() || !creds["salt"].is_string() || !creds["password"].is_string()) {
          result.warnings.push_back("web credentials: no username/password in the credentials file, skipped");
        } else {
          auto doc = target_doc(tgt_loc.credentials);
          if (!doc) {
            return false;
          }
          for (auto key : {"username", "salt", "password"}) {
            (*doc)[key] = creds[key];
          }
          imported.push_back("web credentials");
        }
      }

      for (auto &[key, doc] : target_docs) {
        texts.push_back({target_doc_paths[key], doc.dump(4)});
      }

      if (imported.empty()) {
        result.error = "nothing was imported";
        return false;
      }

      // Nothing may be written into the other host's directory.
      auto inside_source = [&](const fs::path &p) {
        return relative_below(to_u8(p.lexically_normal()), source_norm).has_value();
      };
      for (auto &t : texts) {
        if (inside_source(t.dest)) {
          result.error = "refusing to write inside the source directory: " + to_u8(t.dest);
          return false;
        }
      }
      for (auto &c : copies) {
        if (inside_source(c.dest)) {
          result.error = "refusing to write inside the source directory: " + to_u8(c.dest);
          return false;
        }
      }

      // ---- commit ----
      fs::create_directories(target, ec);
      if (ec) {
        result.error = "cannot create target directory " + to_u8(target) + ": " + ec.message();
        return false;
      }
      backup_t backup(target);
      std::string error;
      for (auto &c : copies) {
        if (!copy_atomic(c.from, c.dest, c.secret, backup, error)) {
          result.backup_dir = backup.dir();
          result.error = error;
          return false;
        }
      }
      for (auto &t : texts) {
        if (!write_atomic(t.dest, t.content, backup, error)) {
          result.backup_dir = backup.dir();
          result.error = error;
          return false;
        }
      }

      result.backup_dir = backup.dir();
      result.imported = std::move(imported);
      return true;
    }

    std::optional<source_t> describe(const fs::path &dir) {
      if (!is_dir(dir)) {
        return std::nullopt;
      }
      auto entries = load_conf(dir);
      auto loc = resolve_locations(dir, entries);

      source_t s;
      s.product = guess_product(dir);
      s.config_dir = dir;
      s.has_settings = is_file(loc.conf);
      s.has_apps = is_file(loc.apps);
      s.has_state = is_file(loc.state);
      s.has_identity = is_file(loc.pkey) && is_file(loc.cert);
      if (!s.has_settings && !s.has_apps && !s.has_state) {
        return std::nullopt;
      }
      return s;
    }
  }  // namespace

  std::optional<source_t> inspect_source(const fs::path &config_dir) {
    try {
      return describe(config_dir);
    } catch (...) {
      return std::nullopt;
    }
  }

  std::vector<source_t> detect_sources(const fs::path &exclude) {
    std::vector<source_t> found;
    try {
      std::vector<fs::path> candidates;
#ifdef _WIN32
      for (auto var : {get_env(nullptr, L"ProgramFiles"), get_env(nullptr, L"ProgramW6432")}) {
        if (!var.empty()) {
          candidates.push_back(var / "Apollo" / "config");
          candidates.push_back(var / "Sunshine" / "config");
        }
      }
#else
      fs::path base = get_env("XDG_CONFIG_HOME", nullptr);
      if (base.empty()) {
        if (auto home = get_env("HOME", nullptr); !home.empty()) {
          base = home / ".config";
        }
      }
      if (!base.empty()) {
        candidates.push_back(base / "sunshine");
        candidates.push_back(base / "apollo");
      }
      // macOS keeps these under ~/.config as well.
      if (auto home = get_env("HOME", nullptr); !home.empty()) {
        candidates.push_back(home / ".config" / "sunshine");
        candidates.push_back(home / ".config" / "apollo");
      }
#endif

      std::error_code ec;
      for (auto &dir : candidates) {
        auto source = describe(dir);
        if (!source) {
          continue;
        }
        if (!exclude.empty() && same_directory(dir, exclude)) {
          continue;
        }
        bool duplicate = std::any_of(found.begin(), found.end(), [&](const source_t &f) {
          return same_directory(f.config_dir, dir);
        });
        if (!duplicate) {
          found.push_back(std::move(*source));
        }
      }
    } catch (...) {
      // Detection is best effort.
    }
    return found;
  }

  result_t import_from(const fs::path &source_config_dir, const fs::path &target_config_dir, const options_t &options) {
    result_t result;
    try {
      result.ok = run_import(source_config_dir, target_config_dir, options, result);
      if (!result.ok && result.error.empty()) {
        result.error = "import failed";
      }
    } catch (const std::exception &e) {
      result.ok = false;
      result.error = "import failed: "s + e.what();
    } catch (...) {
      result.ok = false;
      result.error = "import failed";
    }
    if (!result.ok) {
      result.imported.clear();
    }
    return result;
  }
}  // namespace config_import
