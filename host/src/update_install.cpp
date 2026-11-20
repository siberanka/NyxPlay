/**
 * @file src/update_install.cpp
 * @brief Definitions for the one-click installation of a new NyxPlay Host release.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "release_verify.h"
#include "rtsp.h"
#include "system_tray.h"
#include "tools/svc_logic.h"
#include "update_install.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <Windows.h>
  #include <aclapi.h>
  #include <sddl.h>
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace update_install {
  const char *to_string(state_e state) {
    switch (state) {
      case state_e::idle:
        return "idle";
      case state_e::available:
        return "available";
      case state_e::downloading:
        return "downloading";
      case state_e::verifying:
        return "verifying";
      case state_e::waiting_for_stream:
        return "waiting_for_stream";
      case state_e::installing:
        return "installing";
      case state_e::failed:
        return "failed";
    }
    return "idle";
  }

  const char *to_string(error_e error) {
    switch (error) {
      case error_e::none:
        return "none";
      case error_e::no_connection:
        return "no_connection";
      case error_e::verify_failed:
        return "verify_failed";
      case error_e::no_space:
        return "no_space";
      case error_e::cancelled:
        return "cancelled";
      case error_e::not_supported:
        return "not_supported";
      case error_e::launch_failed:
        return "launch_failed";
      case error_e::installer_failed:
        return "installer_failed";
      case error_e::internal:
        return "internal";
    }
    return "internal";
  }

  // ---- pure helpers ------------------------------------------------------------------------------------------

  namespace {
    bool is_version_char(char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
    }

    struct url_parts_t {
      std::string host;
      std::string path;  ///< Starts with '/', query included.
    };

    /// Strict split of an `https://host/path` URL. No user info, no port, no control characters.
    std::optional<url_parts_t> split_url(std::string_view url) {
      constexpr auto scheme = "https://"sv;
      if (url.size() > 2048 || url.substr(0, scheme.size()) != scheme) {
        return std::nullopt;
      }
      for (const char ch : url) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7f || ch == '\\') {
          return std::nullopt;
        }
      }
      url.remove_prefix(scheme.size());
      const auto slash = url.find('/');
      if (slash == std::string_view::npos || slash == 0) {
        return std::nullopt;
      }
      url_parts_t parts {std::string {url.substr(0, slash)}, std::string {url.substr(slash)}};
      if (parts.host.find_first_of("@:[]%#?") != std::string::npos) {
        return std::nullopt;
      }
      std::transform(parts.host.begin(), parts.host.end(), parts.host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (parts.path.find('#') != std::string::npos) {
        return std::nullopt;
      }
      return parts;
    }

    bool path_is_clean(std::string_view path) {
      std::string lower;
      for (const char ch : path.substr(0, path.find('?'))) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
      return lower.find("%2e") == std::string::npos && lower.find("%2f") == std::string::npos &&
             lower.find("%5c") == std::string::npos && lower.find("%00") == std::string::npos &&
             lower.find("//") == std::string::npos && lower.find("/./") == std::string::npos &&
             lower.find("/../") == std::string::npos && !lower.ends_with("/.") && !lower.ends_with("/..");
    }
  }  // namespace

  std::optional<std::string> version_text(std::string_view tag) {
    if (!update_check::parse_version(tag, true)) {
      return std::nullopt;
    }
    if (tag.size() >= 5 && std::equal(tag.begin(), tag.begin() + 5, "host-")) {
      tag.remove_prefix(5);
    }
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) {
      tag.remove_prefix(1);
    }
    if (tag.empty() || !std::all_of(tag.begin(), tag.end(), is_version_char)) {
      return std::nullopt;
    }
    return std::string {tag};
  }

  std::string installer_name(std::string_view version) {
    return "NyxPlayHost-" + std::string {version} + "-windows-x64.exe";
  }

  std::vector<std::string> asset_urls(std::string_view tag, std::string_view asset) {
    std::vector<std::string> urls;
    if (!version_text(tag) || asset.empty() || !std::all_of(asset.begin(), asset.end(), [](char c) {
          return is_version_char(c) || c == '_';
        })) {
      return urls;
    }
    urls.push_back("https://github.com/siberanka/NyxPlay/releases/download/" + std::string {tag} + "/" + std::string {asset});
    urls.push_back("https://gitlab.com/siberanka/NyxPlay/-/releases/" + std::string {tag} + "/downloads/" + std::string {asset});
    return urls;
  }

  bool is_allowed_url(std::string_view url) {
    const auto parts = split_url(url);
    if (!parts || !path_is_clean(parts->path)) {
      return false;
    }
    if (parts->host == "github.com") {
      return parts->path.starts_with("/siberanka/NyxPlay/releases/download/");
    }
    if (parts->host == "gitlab.com") {
      return parts->path.starts_with("/siberanka/NyxPlay/");
    }
    // Signed storage URLs behind the GitHub release downloads
    return parts->host == "objects.githubusercontent.com" || parts->host == "release-assets.githubusercontent.com";
  }

  std::optional<bool> parse_request(std::string_view body) {
    if (body.size() > 1024) {
      return std::nullopt;
    }
    if (std::all_of(body.begin(), body.end(), [](unsigned char c) {
          return std::isspace(c);
        })) {
      return false;
    }
    const auto root = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
      return std::nullopt;
    }
    bool now = false;
    for (const auto &[key, value] : root.items()) {
      if (key != "now" || !value.is_boolean()) {
        return std::nullopt;
      }
      now = value.get<bool>();
    }
    return now;
  }

  const char *updates_dir_sddl() {
    return "D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  }

  std::optional<std::string> updated_notice(std::string_view marker, std::string_view current) {
    const auto space = marker.find(' ');
    if (space == std::string_view::npos) {
      return std::nullopt;
    }
    const auto previous = update_check::parse_version(marker.substr(0, space), false);
    const auto now = update_check::parse_version(current, false);
    if (!previous || !now || update_check::compare(*now, *previous) <= 0) {
      return std::nullopt;
    }
    return std::string {current};
  }

  // ---- controller --------------------------------------------------------------------------------------------

  namespace {
    std::string read_small_file(const fs::path &path, std::uint64_t max_bytes) {
      std::error_code ec;
      const auto size = fs::file_size(path, ec);
      if (ec || size > max_bytes) {
        return {};
      }
      std::ifstream in(path, std::ios::binary);
      std::ostringstream out;
      out << in.rdbuf();
      return out.str();
    }

    error_e to_error(transfer_e result) {
      switch (result) {
        case transfer_e::ok:
          return error_e::none;
        case transfer_e::network:
          return error_e::no_connection;
        case transfer_e::no_space:
          return error_e::no_space;
        case transfer_e::cancelled:
          return error_e::cancelled;
        case transfer_e::too_large:
        case transfer_e::rejected:
          return error_e::verify_failed;
      }
      return error_e::internal;
    }
  }  // namespace

  controller_t::controller_t(environment_t environment):
      env {std::move(environment)} {
  }

  controller_t::~controller_t() {
    shutdown();
  }

  bool controller_t::cancelled() const {
    return cancel_flag.load();
  }

  void controller_t::set_state(state_e state, error_e error, bool notify) {
    status_t snapshot;
    {
      std::lock_guard lock {mutex};
      current.state = state;
      current.error = error;
      if (state == state_e::downloading && error == error_e::none && notify) {
        current.percent = 0;
      }
      snapshot = current;
    }
    if (env.publish) {
      env.publish(snapshot, notify);
    }
  }

  void controller_t::fail(error_e error) {
    cleanup_files();
    if (error == error_e::cancelled) {
      BOOST_LOG(info) << "Update installation cancelled"sv;
      set_state(state_e::available, error_e::none);
      return;
    }
    BOOST_LOG(warning) << "Update installation failed: "sv << to_string(error);
    set_state(state_e::failed, error, true);
  }

  void controller_t::cleanup_files() {
    std::error_code ec;
    if (env.updates_dir.empty() || !fs::is_directory(env.updates_dir, ec)) {
      return;
    }
    std::string version;
    {
      std::lock_guard lock {mutex};
      version = current.version;
    }
    for (const auto &name : {installer_name(version), installer_name(version) + ".part", "SHA256SUMS.txt"s, "SHA256SUMS.txt.part"s, "SHA256SUMS.txt.sig"s, "SHA256SUMS.txt.sig.part"s}) {
      fs::remove(env.updates_dir / name, ec);
    }
  }

  void controller_t::set_candidate(const update_check::release_t &release) {
    const auto version = version_text(release.tag);
    if (!version) {
      return;
    }
    status_t snapshot;
    {
      std::lock_guard lock {mutex};
      if (current.state == state_e::downloading || current.state == state_e::verifying || current.state == state_e::waiting_for_stream || current.state == state_e::installing) {
        return;
      }
      const bool changed = candidate_tag != release.tag;
      candidate_tag = release.tag;
      current.version = *version;
      current.release_url = release.url;
      if (changed || current.state == state_e::idle) {
        current.state = state_e::available;
        current.error = error_e::none;
        current.percent = 0;
      }
      snapshot = current;
    }
    if (env.publish) {
      env.publish(snapshot, false);
    }
  }

  request_e controller_t::request_install(bool now) {
    std::lock_guard lock {mutex};
    if (stopping || candidate_tag.empty()) {
      return request_e::no_update;
    }
    switch (current.state) {
      case state_e::installing:
        return request_e::busy_installing;
      case state_e::downloading:
      case state_e::verifying:
        return request_e::already_running;
      case state_e::waiting_for_stream:
        if (now) {
          install_now = true;
          cv.notify_all();
        }
        return request_e::already_running;
      case state_e::idle:
        return request_e::no_update;
      case state_e::available:
      case state_e::failed:
        break;
    }

    if (worker.joinable()) {
      worker.join();  // the previous run has ended: its state is final
    }
    cancel_flag = false;
    install_now = now;
    notified_bucket = -1;
    current.state = state_e::downloading;
    current.error = error_e::none;
    current.percent = 0;
    try {
      worker = std::thread([this] {
        run();
      });
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Could not start the update installation: "sv << e.what();
      current.state = state_e::failed;
      current.error = error_e::internal;
      return request_e::no_update;
    }
    return request_e::started;
  }

  bool controller_t::cancel() {
    std::lock_guard lock {mutex};
    if (current.state != state_e::downloading && current.state != state_e::verifying && current.state != state_e::waiting_for_stream) {
      return false;
    }
    cancel_flag = true;
    cv.notify_all();
    return true;
  }

  status_t controller_t::status() const {
    status_t snapshot;
    {
      std::lock_guard lock {mutex};
      snapshot = current;
    }
    snapshot.stream_active = env.stream_active && env.stream_active();
    snapshot.can_install = env.can_install && env.can_install();
    return snapshot;
  }

  void controller_t::shutdown() {
    std::thread to_join;
    {
      std::lock_guard lock {mutex};
      stopping = true;
      cancel_flag = true;
      to_join = std::move(worker);
    }
    cv.notify_all();
    if (to_join.joinable()) {
      to_join.join();
    }
  }

  void controller_t::run() {
    std::string tag;
    std::string version;
    {
      std::lock_guard lock {mutex};
      tag = candidate_tag;
      version = current.version;
    }
    const auto name = installer_name(version);
    const auto installer = env.updates_dir / name;
    const auto sums_path = env.updates_dir / "SHA256SUMS.txt";
    const auto sig_path = env.updates_dir / "SHA256SUMS.txt.sig";

    BOOST_LOG(info) << "Installing NyxPlay Host "sv << version << ": downloading"sv;

    if (!env.can_install || !env.can_install()) {
      return fail(error_e::not_supported);
    }
    if (!env.prepare_dir || !env.prepare_dir(env.updates_dir)) {
      return fail(error_e::internal);
    }
    cleanup_files();
    set_state(state_e::downloading, error_e::none, true);

    // Tries the GitHub release first and the mirror second; a verification problem is never retried elsewhere
    auto fetch = [&](const std::string &asset, const fs::path &dest, std::uint64_t max_bytes, const progress_fn &progress) {
      auto result = transfer_e::rejected;
      for (const auto &url : asset_urls(tag, asset)) {
        result = env.download(url, dest, max_bytes, cancel_flag, progress);
        if (result == transfer_e::ok || result == transfer_e::cancelled || result == transfer_e::no_space) {
          break;
        }
      }
      return result;
    };

    if (auto r = fetch("SHA256SUMS.txt", sums_path, max_sums_bytes, nullptr); r != transfer_e::ok) {
      return fail(to_error(r));
    }
    if (auto r = fetch("SHA256SUMS.txt.sig", sig_path, max_signature_bytes, nullptr); r != transfer_e::ok) {
      return fail(to_error(r));
    }

    // The signature comes first: nothing else of the release is trusted before it checks out
    const auto sums = read_small_file(sums_path, max_sums_bytes);
    const auto signature = read_small_file(sig_path, max_signature_bytes);
    if (const auto r = release_verify::verify_signature(sums, signature); r != release_verify::result_e::ok) {
      BOOST_LOG(warning) << "The release signature was rejected: "sv << release_verify::to_string(r);
      return fail(error_e::verify_failed);
    }
    std::string expected_hash;
    if (const auto r = release_verify::find_hash(sums, name, expected_hash); r != release_verify::result_e::ok) {
      BOOST_LOG(warning) << "The signed list has no usable entry for the installer: "sv << release_verify::to_string(r);
      return fail(error_e::verify_failed);
    }

    auto progress = [&](std::uint64_t received, std::uint64_t total) {
      if (total == 0) {
        return;
      }
      const int percent = static_cast<int>(std::min<std::uint64_t>(100, received * 100 / total));
      status_t snapshot;
      bool notify = false;
      {
        std::lock_guard lock {mutex};
        if (percent == current.percent) {
          return;
        }
        current.percent = percent;
        if (percent >= 50 && notified_bucket < 1) {
          notified_bucket = 1;
          notify = true;
        }
        snapshot = current;
      }
      if (env.publish) {
        env.publish(snapshot, notify);
      }
    };
    if (auto r = fetch(name, installer, max_installer_bytes, progress); r != transfer_e::ok) {
      return fail(to_error(r));
    }

    set_state(state_e::verifying, error_e::none, true);
    if (cancelled()) {
      return fail(error_e::cancelled);
    }
    if (const auto r = release_verify::verify_file(sums, name, installer); r != release_verify::result_e::ok) {
      BOOST_LOG(warning) << "The installer was rejected: "sv << release_verify::to_string(r);
      return fail(error_e::verify_failed);
    }

    // A running stream is never interrupted without being asked
    bool waited = false;
    for (;;) {
      if (cancelled()) {
        return fail(error_e::cancelled);
      }
      bool hurry;
      {
        std::lock_guard lock {mutex};
        hurry = install_now;
      }
      if (hurry || !env.stream_active || !env.stream_active()) {
        break;
      }
      if (!waited) {
        waited = true;
        BOOST_LOG(info) << "The update is ready and waits for the stream to end"sv;
        set_state(state_e::waiting_for_stream, error_e::none, true);
      }
      std::unique_lock lock {mutex};
      cv.wait_for(lock, env.poll_interval, [&] {
        return cancel_flag.load() || install_now;
      });
    }

    if (waited) {
      // The file sat on disk for a while: check it again right before it is run
      if (const auto r = release_verify::verify_file(sums, name, installer); r != release_verify::result_e::ok) {
        return fail(error_e::verify_failed);
      }
    }

    env.store_marker(env.current_version + " " + version);
    BOOST_LOG(info) << "Handing the verified installer to the service"sv;
    set_state(state_e::installing, error_e::none, true);

    const auto launched = env.launch(installer, expected_hash);
    if (launched != launch_e::ok) {
      env.store_marker({});
      return fail(launched == launch_e::not_supported ? error_e::not_supported : error_e::launch_failed);
    }

    // The installer stops this process when it succeeds. Only an early failure is seen here.
    const auto deadline = std::chrono::steady_clock::now() + env.install_timeout;
    for (;;) {
      if (const auto code = env.install_result ? env.install_result() : std::nullopt) {
        BOOST_LOG(warning) << "The installer ended with exit code "sv << *code;
        env.store_marker({});
        return fail(error_e::installer_failed);
      }
      std::unique_lock lock {mutex};
      if (cv.wait_for(lock, env.poll_interval, [&] {
            return stopping;
          })) {
        return;  // the host is shutting down: the installer may be running, leave its files alone
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        lock.unlock();
        env.store_marker({});
        return fail(error_e::installer_failed);
      }
    }
  }

  // ---- host integration --------------------------------------------------------------------------------------

  namespace {
    std::unique_ptr<controller_t> instance;
    std::mutex instance_mutex;

    transfer_e map_curl(CURLcode code) {
      switch (code) {
        case CURLE_ABORTED_BY_CALLBACK:
          return transfer_e::cancelled;
        case CURLE_WRITE_ERROR:
          return transfer_e::no_space;
        default:
          return transfer_e::network;
      }
    }

    struct sink_t {
      std::ofstream out;
      CURL *curl = nullptr;
      const std::atomic<bool> *cancel = nullptr;
      const progress_fn *progress = nullptr;
      std::uint64_t max_bytes = 0;
      std::uint64_t written = 0;
      fs::path dir;
      bool too_large = false;
      bool no_space = false;
      bool space_checked = false;
      std::string location;
    };

    long response_code(CURL *curl) {
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      return status;
    }

    std::size_t write_cb(char *data, std::size_t size, std::size_t count, void *user) {
      auto *sink = static_cast<sink_t *>(user);
      const std::size_t bytes = size * count;
      if (response_code(sink->curl) != 200) {
        return bytes;  // body of a redirect or an error page
      }
      if (sink->written + bytes > sink->max_bytes) {
        sink->too_large = true;
        return 0;
      }
      sink->out.write(data, static_cast<std::streamsize>(bytes));
      if (!sink->out.good()) {
        sink->no_space = true;
        return 0;
      }
      sink->written += bytes;
      return bytes;
    }

    std::size_t header_cb(char *data, std::size_t size, std::size_t count, void *user) {
      auto *sink = static_cast<sink_t *>(user);
      const std::string_view line {data, size * count};
      if (line.starts_with("HTTP/")) {
        sink->location.clear();
      } else if (line.size() > 9 && std::equal(line.begin(), line.begin() + 9, "location:", [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
                 })) {
        auto value = line.substr(9);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
          value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
          value.remove_suffix(1);
        }
        sink->location = std::string {value};
      }
      return size * count;
    }

    int xfer_cb(void *user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
      auto *sink = static_cast<sink_t *>(user);
      if (sink->cancel && sink->cancel->load()) {
        return 1;
      }
      if (response_code(sink->curl) != 200) {
        return 0;
      }
      if (total > 0 && static_cast<std::uint64_t>(total) > sink->max_bytes) {
        sink->too_large = true;
        return 1;
      }
      if (total > 0 && !sink->space_checked) {
        sink->space_checked = true;
        std::error_code ec;
        const auto space = fs::space(sink->dir, ec);
        // Room for the file and the installer's own working set
        if (!ec && space.available < static_cast<std::uintmax_t>(total) + 64ull * 1024 * 1024) {
          sink->no_space = true;
          return 1;
        }
      }
      if (sink->progress && *sink->progress && total > 0) {
        (*sink->progress)(static_cast<std::uint64_t>(now), static_cast<std::uint64_t>(total));
      }
      return 0;
    }

    transfer_e download_file(std::string_view start_url, const fs::path &dest, std::uint64_t max_bytes, const std::atomic<bool> &cancel, const progress_fn &progress) {
      auto part = dest;
      part += ".part";
      std::error_code ec;
      fs::remove(part, ec);

      std::string url {start_url};
      for (int hop = 0; hop <= max_redirects; ++hop) {
        if (!is_allowed_url(url)) {
          BOOST_LOG(warning) << "A download location was refused"sv;
          return transfer_e::rejected;
        }

        sink_t sink;
        sink.cancel = &cancel;
        sink.progress = &progress;
        sink.max_bytes = max_bytes;
        sink.dir = dest.parent_path();
        sink.out.open(part, std::ios::binary | std::ios::trunc);
        if (!sink.out) {
          return transfer_e::no_space;
        }

        CURL *curl = curl_easy_init();  // NOSONAR
        if (!curl) {
          return transfer_e::network;
        }
        sink.curl = curl;
        const std::string user_agent = "NyxPlay-Host/"s + PROJECT_VERSION;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);  // every redirect target is checked here
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);  // NOSONAR
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L * 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
  #ifdef _WIN32
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
  #endif

        const CURLcode code = curl_easy_perform(curl);
        const long status = response_code(curl);
        curl_easy_cleanup(curl);
        sink.out.close();

        if (sink.too_large) {
          fs::remove(part, ec);
          return transfer_e::too_large;
        }
        if (sink.no_space) {
          fs::remove(part, ec);
          return transfer_e::no_space;
        }
        if (cancel.load()) {
          fs::remove(part, ec);
          return transfer_e::cancelled;
        }
        if (code != CURLE_OK) {
          fs::remove(part, ec);
          return map_curl(code);
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
          fs::remove(part, ec);
          auto next = sink.location;
          if (next.starts_with('/')) {
            const auto parts = split_url(url);
            if (!parts) {
              return transfer_e::rejected;
            }
            next = "https://" + parts->host + next;
          }
          url = std::move(next);
          continue;
        }
        if (status != 200) {
          fs::remove(part, ec);
          return transfer_e::network;
        }

        fs::rename(part, dest, ec);
        if (ec) {
          fs::remove(part, ec);
          return transfer_e::no_space;
        }
        return transfer_e::ok;
      }
      return transfer_e::rejected;  // too many redirects
    }

  #ifdef _WIN32
    HANDLE installer_guard = INVALID_HANDLE_VALUE;

    bool is_elevated() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
      }
      TOKEN_ELEVATION elevation {};
      DWORD size = 0;
      const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
      CloseHandle(token);
      return ok;
    }

    HANDLE open_service_event() {
      const auto name = svc::update_event_name;
      return OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    }

    bool prepare_dir_windows(const fs::path &dir) {
      std::error_code ec;
      fs::create_directories(dir, ec);
      if (ec) {
        return false;
      }
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
      }
      BOOL present = FALSE;
      BOOL defaulted = FALSE;
      PACL dacl = nullptr;
      bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present;
      if (ok) {
        const auto wide = dir.wstring();
        ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
      }
      LocalFree(descriptor);
      return ok;
    }
  #endif

    bool write_text_atomic(const fs::path &path, const std::string &text) {
      auto temp = path;
      temp += ".tmp";
      {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out.good()) {
          return false;
        }
      }
      std::error_code ec;
      fs::rename(temp, path, ec);
      return !ec;
    }

    launch_e launch_via_service(const fs::path &installer, const std::string &sha256) {
  #ifdef _WIN32
      const auto name = installer.filename().string();
      if (!svc::is_installer_name(name)) {
        return launch_e::failed;
      }
      // Keep the verified file from being replaced or deleted until the service has hashed and started it
      if (installer_guard != INVALID_HANDLE_VALUE) {
        CloseHandle(installer_guard);
      }
      installer_guard = CreateFileW(installer.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (installer_guard == INVALID_HANDLE_VALUE) {
        return launch_e::failed;
      }
      const auto dir = installer.parent_path();
      std::error_code ec;
      fs::remove(dir / svc::update_result_file, ec);
      if (!write_text_atomic(dir / svc::update_request_file, name + "\n" + sha256 + "\n")) {
        return launch_e::failed;
      }
      HANDLE event = open_service_event();
      if (!event) {
        BOOST_LOG(warning) << "The host service does not accept update requests (error "sv << GetLastError() << ')';
        return launch_e::not_supported;
      }
      const bool signalled = SetEvent(event);
      CloseHandle(event);
      return signalled ? launch_e::ok : launch_e::failed;
  #else
      (void) installer;
      (void) sha256;
      return launch_e::not_supported;
  #endif
    }

    std::optional<int> read_install_result(const fs::path &dir) {
      const auto text = read_small_file(dir / svc::update_result_file, 64);
      if (text.empty()) {
        return std::nullopt;
      }
      try {
        return std::stoi(text);
      } catch (...) {
        return 1;
      }
    }

    void remove_stale(const fs::path &dir, bool everything) {
      std::error_code ec;
      if (!fs::is_directory(dir, ec)) {
        return;
      }
      const auto now = fs::file_time_type::clock::now();
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name == "install.log") {
          continue;
        }
        std::error_code entry_ec;
        const auto written = entry.last_write_time(entry_ec);
        if (everything || (!entry_ec && now - written > stale_after)) {
          fs::remove_all(entry.path(), entry_ec);
        }
      }
    }
  }  // namespace

  void init() {
    std::lock_guard lock {instance_mutex};
    if (instance) {
      return;
    }

    environment_t env;
    env.updates_dir = platf::appdata() / "updates";
    env.current_version = PROJECT_VERSION;
    env.download = download_file;
    env.stream_active = [] {
      return rtsp_stream::session_count() > 0;
    };
  #ifdef _WIN32
    env.prepare_dir = prepare_dir_windows;
    env.can_install = [] {
      if (!is_elevated()) {
        return false;
      }
      HANDLE event = open_service_event();
      if (!event) {
        return false;
      }
      CloseHandle(event);
      return true;
    };
  #else
    env.prepare_dir = [](const fs::path &dir) {
      std::error_code ec;
      fs::create_directories(dir, ec);
      return !ec;
    };
    env.can_install = [] {
      return false;
    };
  #endif
    env.launch = launch_via_service;
    const auto dir = env.updates_dir;
    env.install_result = [dir] {
      return read_install_result(dir);
    };
    env.store_marker = [](std::string_view marker) {
      nvhttp::store_state_text("update_marker", marker);
    };
    env.publish = [](const status_t &status, bool notify) {
  #if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      if (!config::sunshine.system_tray) {
        return;
      }
      system_tray::update_ui_t ui;
      switch (status.state) {
        case state_e::idle:
          ui.state = system_tray::update_ui_e::none;
          break;
        case state_e::available:
          ui.state = system_tray::update_ui_e::available;
          break;
        case state_e::downloading:
          ui.state = system_tray::update_ui_e::downloading;
          break;
        case state_e::verifying:
          ui.state = system_tray::update_ui_e::verifying;
          break;
        case state_e::waiting_for_stream:
          ui.state = system_tray::update_ui_e::waiting;
          break;
        case state_e::installing:
          ui.state = system_tray::update_ui_e::installing;
          break;
        case state_e::failed:
          ui.state = system_tray::update_ui_e::failed;
          break;
      }
      ui.version = status.version;
      ui.percent = status.percent;
      ui.reason = to_string(status.error);
      ui.url = status.release_url;
      ui.notify = notify;
      system_tray::update_tray_update_state(std::move(ui));
  #else
      (void) status;
      (void) notify;
  #endif
    };

    // Files of an earlier attempt are not needed any more; a finished installer may still be running, so
    // whatever cannot be removed now is removed by the age rule on the next start
    const auto marker = nvhttp::load_state_text("update_marker");
    if (!marker.empty()) {
      nvhttp::store_state_text("update_marker", {});
      if (const auto version = updated_notice(marker, PROJECT_VERSION)) {
        BOOST_LOG(info) << "NyxPlay Host was updated to "sv << *version;
  #if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
        if (config::sunshine.system_tray) {
          system_tray::update_ui_t ui;
          ui.state = system_tray::update_ui_e::updated;
          ui.version = *version;
          ui.notify = true;
          system_tray::update_tray_update_state(std::move(ui));
        }
  #endif
      } else {
        BOOST_LOG(warning) << "The update did not change the installed version; see the installer log in the updates folder"sv;
      }
      remove_stale(env.updates_dir, true);
    } else {
      remove_stale(env.updates_dir, false);
    }

    instance = std::make_unique<controller_t>(std::move(env));
  }

  void shutdown() {
    std::unique_ptr<controller_t> old;
    {
      std::lock_guard lock {instance_mutex};
      old = std::move(instance);
    }
    old.reset();
  }

  void set_candidate(const update_check::release_t &release) {
    std::lock_guard lock {instance_mutex};
    if (instance) {
      instance->set_candidate(release);
    }
  }

  request_e request_install(bool now) {
    std::lock_guard lock {instance_mutex};
    return instance ? instance->request_install(now) : request_e::no_update;
  }

  bool cancel() {
    std::lock_guard lock {instance_mutex};
    return instance && instance->cancel();
  }

  status_t status() {
    std::lock_guard lock {instance_mutex};
    return instance ? instance->status() : status_t {};
  }
}  // namespace update_install
