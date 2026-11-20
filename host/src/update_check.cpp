/**
 * @file src/update_check.cpp
 * @brief Definitions for the background check for new NyxPlay Host releases.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <thread>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "logging.h"
#include "system_tray.h"
#include "update_check.h"
#include "update_install.h"

using namespace std::literals;

namespace update_check {
  namespace {
    constexpr auto first_check_delay = 2min;
    constexpr auto check_interval = 24h;

    bool starts_with_icase(std::string_view text, std::string_view prefix) {
      return text.size() >= prefix.size() &&
             std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
             });
    }

    bool read_number(std::string_view &text, std::size_t min_digits, std::size_t max_digits, long &out) {
      std::size_t count = 0;
      long value = 0;
      while (count < text.size() && std::isdigit(static_cast<unsigned char>(text[count]))) {
        if (count >= max_digits) {
          return false;
        }
        value = value * 10 + (text[count] - '0');
        ++count;
      }
      if (count < min_digits) {
        return false;
      }
      text.remove_prefix(count);
      out = value;
      return true;
    }

    bool json_bool(const nlohmann::json &object, const char *key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_boolean() && it->get<bool>();
    }

    std::string json_string(const nlohmann::json &object, const char *key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_string() ? it->get<std::string>() : std::string {};
    }

    /// Release lists are shallow; refuse anything nested deeper than a sane limit before parsing it.
    bool nesting_is_sane(std::string_view body) {
      constexpr int max_depth = 16;
      int depth = 0;
      bool in_string = false;
      bool escaped = false;
      for (const char ch : body) {
        if (in_string) {
          if (escaped) {
            escaped = false;
          } else if (ch == '\\') {
            escaped = true;
          } else if (ch == '"') {
            in_string = false;
          }
        } else if (ch == '"') {
          in_string = true;
        } else if (ch == '[' || ch == '{') {
          if (++depth > max_depth) {
            return false;
          }
        } else if (ch == ']' || ch == '}') {
          --depth;
        }
      }
      return true;
    }

    bool is_newer(const release_t &candidate, const std::optional<release_t> &best) {
      return !best || compare(candidate.version, best->version) > 0;
    }
  }  // namespace

  std::optional<version_t> parse_version(std::string_view text, bool strict) {
    if (text.empty() || text.size() > 128) {
      return std::nullopt;
    }

    if (starts_with_icase(text, "host-"sv)) {
      text.remove_prefix(5);
    }
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
      text.remove_prefix(1);
    }

    version_t version;
    for (int i = 0; i < 4; ++i) {
      if (i > 0) {
        if (text.empty() || text.front() != '.') {
          if (i == 3 && !strict) {
            version.parts[3] = 0;
            break;
          }
          return std::nullopt;
        }
        text.remove_prefix(1);
      }

      const std::size_t max_digits = i == 0 ? 4 : (i == 3 ? 9 : 2);
      const std::size_t min_digits = i == 0 && strict ? 4 : 1;
      if (!read_number(text, min_digits, max_digits, version.parts[i])) {
        return std::nullopt;
      }
    }

    if (text.empty()) {
      return version;
    }

    if (text.front() == '-') {
      text.remove_prefix(1);
      long kind;
      if (starts_with_icase(text, "alpha"sv)) {
        kind = 0;
        text.remove_prefix(5);
      } else if (starts_with_icase(text, "beta"sv)) {
        kind = 1;
        text.remove_prefix(4);
      } else if (starts_with_icase(text, "rc"sv)) {
        kind = 2;
        text.remove_prefix(2);
      } else {
        return std::nullopt;
      }

      long number = 0;
      if (!text.empty() && text.front() == '.') {
        text.remove_prefix(1);
        if (!read_number(text, 1, 5, number)) {
          return std::nullopt;
        }
      }
      version.pre_rank = kind * 65536 + number;

      if (!text.empty() && (strict || text.front() != '.')) {
        return std::nullopt;
      }
      return version;
    }

    // A local build appends the commit and a dirty marker to the version
    if (!strict && text.front() == '.') {
      return version;
    }
    return std::nullopt;
  }

  int compare(const version_t &a, const version_t &b) {
    for (std::size_t i = 0; i < a.parts.size(); ++i) {
      if (a.parts[i] != b.parts[i]) {
        return a.parts[i] < b.parts[i] ? -1 : 1;
      }
    }
    if (a.pre_rank != b.pre_rank) {
      return a.pre_rank < b.pre_rank ? -1 : 1;
    }
    return 0;
  }

  std::string safe_release_url(std::string_view url) {
    constexpr std::array prefixes {
      "https://github.com/siberanka/NyxPlay"sv,
      "https://gitlab.com/siberanka/NyxPlay"sv,
    };

    const auto fallback = std::string {releases_page};
    if (url.size() > 512) {
      return fallback;
    }

    const auto prefix = std::find_if(prefixes.begin(), prefixes.end(), [&](std::string_view candidate) {
      return url.substr(0, candidate.size()) == candidate;
    });
    if (prefix == prefixes.end()) {
      return fallback;
    }

    const auto rest = url.substr(prefix->size());
    if (!rest.empty() && rest.front() != '/') {
      return fallback;
    }

    std::string lower;
    for (const char ch : rest) {
      const auto c = static_cast<unsigned char>(ch);
      if (c <= 0x20 || c >= 0x7f || ch == '\\') {
        return fallback;
      }
      lower.push_back(static_cast<char>(std::tolower(c)));
    }
    if (lower.find("%2e") != std::string::npos || lower.find("%2f") != std::string::npos ||
        lower.find("%5c") != std::string::npos || lower.find("%00") != std::string::npos ||
        lower.find("//") != std::string::npos || lower.find("/./") != std::string::npos ||
        lower.find("/../") != std::string::npos || lower.ends_with("/.") || lower.ends_with("/..")) {
      return fallback;
    }

    return std::string {url};
  }

  std::optional<std::vector<release_t>> parse_releases(std::string_view body) {
    if (body.empty() || body.size() > max_body_bytes || !nesting_is_sane(body)) {
      return std::nullopt;
    }

    try {
      const auto root = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
      if (root.is_discarded() || !root.is_array()) {
        return std::nullopt;
      }

      std::vector<release_t> releases;
      std::size_t inspected = 0;
      for (const auto &entry : root) {
        if (++inspected > max_releases) {
          break;
        }
        if (!entry.is_object() || json_bool(entry, "draft")) {
          continue;
        }

        const auto tag = json_string(entry, "tag_name");
        const auto version = parse_version(tag, true);
        if (!version) {
          continue;
        }

        release_t release;
        release.tag = tag;
        release.version = *version;
        release.prerelease = json_bool(entry, "prerelease") || json_bool(entry, "upcoming_release") ||
                             version->pre_rank != version_t::pre_rank_final;

        auto url = json_string(entry, "html_url");
        if (url.empty()) {
          if (const auto links = entry.find("_links"); links != entry.end() && links->is_object()) {
            url = json_string(*links, "self");
          }
        }
        release.url = safe_release_url(url);

        releases.push_back(std::move(release));
      }
      return releases;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<release_t> select_update(const std::vector<release_t> &releases, const version_t &current, bool include_prereleases) {
    std::optional<release_t> stable;
    std::optional<release_t> pre;
    for (const auto &release : releases) {
      auto &best = release.prerelease ? pre : stable;
      if (is_newer(release, best)) {
        best = release;
      }
    }

    std::optional<release_t> result;
    if (stable && compare(stable->version, current) > 0) {
      result = stable;
    }
    if (include_prereleases && pre && compare(pre->version, current) > 0 && (!stable || compare(pre->version, stable->version) > 0)) {
      result = pre;
    }
    return result;
  }

  result_t check(const fetch_fn &fetch, std::string_view current_version, bool include_prereleases) {
    result_t result;

    const auto current = parse_version(current_version, false);
    if (!current) {
      return result;
    }

    bool reachable = false;
    for (const auto url : {github_releases_url, gitlab_releases_url}) {
      std::optional<std::string> body;
      try {
        body = fetch(url);
      } catch (...) {
        body.reset();
      }
      if (!body) {
        continue;
      }

      const auto releases = parse_releases(*body);
      if (!releases) {
        continue;
      }
      reachable = true;
      if (releases->empty()) {
        // Nothing usable here, ask the mirror
        continue;
      }

      if (auto update = select_update(*releases, *current, include_prereleases)) {
        result.status = status_t::update_available;
        result.release = std::move(*update);
      } else {
        result.status = status_t::up_to_date;
      }
      return result;
    }

    result.status = reachable ? status_t::up_to_date : status_t::failed;
    return result;
  }

  bool should_announce(
    const std::string &last_announced_tag,
    std::chrono::system_clock::time_point last_announced_at,
    const std::string &tag,
    std::chrono::system_clock::time_point now
  ) {
    if (last_announced_tag.empty() || last_announced_tag != tag) {
      return true;
    }
    return now < last_announced_at || now - last_announced_at >= reannounce_interval;
  }

  namespace {
    struct download_t {
      std::string body;
      const std::atomic<bool> *cancel;
    };

    std::size_t write_body(char *data, std::size_t size, std::size_t count, void *user) {
      auto *download = static_cast<download_t *>(user);
      const std::size_t bytes = size * count;
      if (download->body.size() + bytes > max_body_bytes) {
        // Returning less than was offered aborts the transfer
        return 0;
      }
      download->body.append(data, bytes);
      return bytes;
    }

    int check_cancel(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      const auto *download = static_cast<download_t *>(user);
      return download->cancel && download->cancel->load() ? 1 : 0;
    }
  }  // namespace

  std::optional<std::string> fetch(
    std::string_view url,
    std::chrono::seconds connect_timeout,
    std::chrono::seconds total_timeout,
    const std::atomic<bool> *cancel
  ) {
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      return std::nullopt;
    }

    download_t download {{}, cancel};
    const std::string url_string {url};
    const std::string user_agent = "NyxPlay-Host/"s + PROJECT_VERSION;
    curl_slist *headers = curl_slist_append(nullptr, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url_string.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);  // NOSONAR
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connect_timeout.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(total_timeout.count()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_cancel);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &download);
#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || status != 200) {
      return std::nullopt;
    }
    return std::move(download.body);
  }

  namespace {
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::thread worker;
    bool stopping = false;
    std::atomic<bool> cancel_requested = false;

    std::string announced_tag;
    std::chrono::system_clock::time_point announced_at;

    /// Wait for the given time. Returns false when the checker should stop.
    bool wait_or_stop(std::chrono::seconds delay) {
      std::unique_lock lock {state_mutex};
      return !state_cv.wait_for(lock, delay, [] {
        return stopping;
      });
    }

    void run_once() {
      const auto result = check(
        [](std::string_view url) {
          return fetch(url, 10s, 20s, &cancel_requested);
        },
        PROJECT_VERSION,
        config::sunshine.notify_pre_releases
      );

      switch (result.status) {
        case status_t::failed:
          BOOST_LOG(debug) << "Update check failed, will retry later"sv;
          break;
        case status_t::up_to_date:
          BOOST_LOG(debug) << "NyxPlay Host is up to date"sv;
          break;
        case status_t::update_available:
          {
            // The installation always acts on the release found here, never on anything a client sends
            update_install::set_candidate(result.release);

            const auto now = std::chrono::system_clock::now();
            if (!should_announce(announced_tag, announced_at, result.release.tag, now)) {
              break;
            }
            announced_tag = result.release.tag;
            announced_at = now;

            BOOST_LOG(info) << "A new NyxPlay Host version is available: "sv << result.release.tag << " ("sv << result.release.url << ')';
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
            if (config::sunshine.system_tray) {
              system_tray::update_tray_update_available(result.release.tag);
            }
#endif
            break;
          }
      }
    }

    void worker_main() {
      if (!wait_or_stop(std::chrono::duration_cast<std::chrono::seconds>(first_check_delay))) {
        return;
      }

      do {
        try {
          run_once();
        } catch (const std::exception &e) {
          BOOST_LOG(debug) << "Update check error: "sv << e.what();
        } catch (...) {
          BOOST_LOG(debug) << "Update check error"sv;
        }
      } while (wait_or_stop(std::chrono::duration_cast<std::chrono::seconds>(check_interval)));
    }
  }  // namespace

  void start() {
    if (!config::sunshine.update_check) {
      BOOST_LOG(debug) << "Update check is disabled"sv;
      return;
    }

    std::lock_guard lock {state_mutex};
    if (worker.joinable()) {
      return;
    }
    stopping = false;
    cancel_requested = false;
    try {
      worker = std::thread(worker_main);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Could not start the update check: "sv << e.what();
    }
  }

  void stop() {
    std::thread to_join;
    {
      std::lock_guard lock {state_mutex};
      stopping = true;
      cancel_requested = true;
      to_join = std::move(worker);
    }
    state_cv.notify_all();
    if (to_join.joinable()) {
      to_join.join();
    }
  }
}  // namespace update_check
