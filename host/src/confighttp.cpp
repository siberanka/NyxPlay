/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTPS server.
 *
 * @todo Better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numeric>
#include <regex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

// lib includes
#include <openssl/crypto.h>
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "config_import.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "security_log.h"
#include "utility.h"
#include "uuid.h"

#ifdef _WIN32
  #include "platform/windows/utils.h"
#endif

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  /**
   * @brief Web UI server that turns blocked addresses away before the TLS handshake.
   * @note The accept() member is Server<HTTPS>::accept() with the address check added.
   */
  class WebUiServer: public SimpleWeb::Server<SimpleWeb::HTTPS> {
  public:
    using SimpleWeb::Server<SimpleWeb::HTTPS>::Server;

  protected:
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        if (!ec && security_log::refuse_if_blocked(connection->socket->lowest_layer(), security_log::service::webui)) {
          return;
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              this->read(session);
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = WebUiServer;
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Keep the base enum for client operations.
  enum class op_e {
    ADD,    ///< Add client
    REMOVE  ///< Remove client
  };

  // SESSION COOKIE
  // The server runs a single worker thread, so the session state and the login throttle below are
  // only touched by one thread. Add a mutex before raising the thread pool size.
  std::string sessionCookie;
  static std::chrono::time_point<std::chrono::steady_clock> cookie_creation_time;

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;
    for (auto &[name, val] : request->header) {
      // The session cookie is as sensitive as the credentials
      const bool secret = boost::iequals(name, "Authorization") || boost::iequals(name, "Cookie");
      BOOST_LOG(debug) << name << " -- " << (secret ? "CREDENTIALS REDACTED"sv : std::string_view {val});
    }
    BOOST_LOG(debug) << " [--] "sv;
    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }
    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Record an administrative action in the security log.
   */
  static void log_admin(const req_https_t &request, std::string_view detail) {
    security_log::record(security_log::type::admin_action, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_name = "Web UI"sv, .detail = detail});
  }

  /**
   * @brief Add the headers every response carries.
   * @param headers The header map to extend.
   * @param cacheable Leave out `Cache-Control: no-store` (static assets only).
   */
  static void add_security_headers(SimpleWeb::CaseInsensitiveMultimap &headers, bool cacheable = false) {
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    headers.emplace("X-Content-Type-Options", "nosniff");
    headers.emplace("Referrer-Policy", "no-referrer");
    if (!cacheable) {
      headers.emplace("Cache-Control", "no-store");
    }
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    add_security_headers(headers);
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a JSON error response.
   * @param response The HTTP response object.
   * @param code The HTTP status code.
   * @param message The error message shown to the caller.
   * @param extra_headers Additional headers.
   */
  static void send_error(resp_https_t response, SimpleWeb::StatusCode code, const std::string &message, SimpleWeb::CaseInsensitiveMultimap extra_headers = {}) {
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = message;
    extra_headers.emplace("Content-Type", "application/json");
    add_security_headers(extra_headers);
    response->write(code, tree.dump(), extra_headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    security_log::record(security_log::type::webui_denied, security_log::result::denied, address, security_log::service::webui, {.detail = "no valid session"sv});
    send_error(response, SimpleWeb::StatusCode::client_error_unauthorized, "Unauthorized");
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- redirecting"sv;
    SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path}
    };
    add_security_headers(headers);
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Retrieve the value of a key from a cookie string.
   * @param cookieString The cookie header string.
   * @param key The key to search.
   * @return The value if found, empty string otherwise.
   */
  std::string getCookieValue(const std::string &cookieString, const std::string &key) {
    // Match whole cookie names only ("xauth=" must not match "auth=")
    std::string_view cookies {cookieString};
    while (!cookies.empty()) {
      auto end = cookies.find(';');
      auto pair = cookies.substr(0, end);
      while (!pair.empty() && pair.front() == ' ') {
        pair.remove_prefix(1);
      }
      if (pair.size() > key.size() && pair.starts_with(key) && pair[key.size()] == '=') {
        auto value = pair.substr(key.size() + 1);
        while (!value.empty() && value.back() == ' ') {
          value.remove_suffix(1);
        }
        return std::string {value};
      }
      if (end == std::string_view::npos) {
        break;
      }
      cookies.remove_prefix(end + 1);
    }
    return "";
  }

  namespace {
    constexpr std::size_t max_origin_header_size = 2048;
    constexpr int max_json_depth = 64;

    /**
     * @brief Lower-case an authority and drop the default HTTPS port.
     */
    std::string normalize_authority(std::string_view authority) {
      while (!authority.empty() && (authority.front() == ' ' || authority.front() == '\t')) {
        authority.remove_prefix(1);
      }
      while (!authority.empty() && (authority.back() == ' ' || authority.back() == '\t')) {
        authority.remove_suffix(1);
      }
      auto text = boost::algorithm::to_lower_copy(std::string {authority});
      if (text.ends_with(":443")) {
        text.resize(text.size() - 4);
      }
      return text;
    }

    /**
     * @brief Authority (host[:port]) of an https URL, empty for anything else (including URLs with credentials).
     */
    std::string https_authority(std::string_view url) {
      constexpr std::string_view scheme {"https://"};
      if (url.size() > max_origin_header_size || url.size() <= scheme.size() || !boost::iequals(url.substr(0, scheme.size()), scheme)) {
        return {};
      }
      url.remove_prefix(scheme.size());
      auto authority = url.substr(0, url.find_first_of("/?#"));
      if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return {};
      }
      return normalize_authority(authority);
    }

    /**
     * @brief Whether the value nests deeper than the limit (stops descending at the limit).
     */
    bool exceeds_depth(const nlohmann::json &value, int limit) {
      if (!value.is_structured()) {
        return false;
      }
      if (limit <= 0) {
        return true;
      }
      return std::any_of(value.begin(), value.end(), [limit](const nlohmann::json &child) {
        return exceeds_depth(child, limit - 1);
      });
    }

    /**
     * @brief Printable, bounded text for log lines.
     */
    std::string loggable(std::string_view text, std::size_t max_size = 128) {
      std::string result {text.substr(0, max_size)};
      for (auto &ch : result) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) {
          ch = '?';
        }
      }
      return result;
    }
  }  // namespace

  bool is_same_origin(std::string_view method, std::string_view host, std::string_view origin, std::string_view referer, std::string_view sec_fetch_site) {
    if (boost::iequals(method, "GET") || boost::iequals(method, "HEAD") || boost::iequals(method, "OPTIONS")) {
      return true;
    }

    // The browser decides this one and scripts cannot change it
    if (!sec_fetch_site.empty()) {
      return boost::iequals(sec_fetch_site, "same-origin") || boost::iequals(sec_fetch_site, "none");
    }

    const auto own_authority = normalize_authority(host);
    const auto matches = [&own_authority](std::string_view url) {
      const auto authority = https_authority(url);
      return !authority.empty() && !own_authority.empty() && authority == own_authority;
    };
    if (!origin.empty()) {
      return matches(origin);
    }
    if (!referer.empty()) {
      return matches(referer);
    }

    // Not a browser: the session cookie is still required
    return true;
  }

  std::size_t body_limit_for(std::string_view method, std::string_view path) {
    constexpr std::size_t small_body = 4 * 1024;
    constexpr std::size_t medium_body = 256 * 1024;
    if (!boost::iequals(method, "POST")) {
      return small_body;
    }
    if (path == "/api/covers/upload") {
      return MAX_COVER_REQUEST_SIZE;
    }
    if (path == "/api/apps" || path == "/api/apps/reorder" || path == "/api/config" || path == "/api/clients/update") {
      return medium_body;
    }
    return small_body;
  }

  int login_delay_seconds(int failures) {
    if (failures <= 0) {
      return 0;
    }
    return std::min(60, 1 << std::min(failures - 1, 6));
  }

  /**
   * @brief Reject a state-changing request that does not come from the Web UI's own origin.
   * @return True if the request may continue.
   */
  static bool check_request_origin(resp_https_t response, req_https_t request) {
    const auto header = [&request](const char *name) -> std::string {
      auto it = request->header.find(name);
      return it == request->header.end() ? std::string {} : it->second;
    };
    if (is_same_origin(request->method, header("host"), header("origin"), header("referer"), header("sec-fetch-site"))) {
      return true;
    }

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    security_log::record(security_log::type::webui_denied, security_log::result::denied, address, security_log::service::webui, {.detail = "cross-origin request"sv});
    BOOST_LOG(warning) << "Web UI: ["sv << address << "] -- cross-origin request rejected (origin ["sv
                       << loggable(header("origin")) << "], referer ["sv << loggable(header("referer")) << "], fetch site ["sv
                       << loggable(header("sec-fetch-site"), 32) << "])"sv;
    send_error(response, SimpleWeb::StatusCode::client_error_forbidden, "Cross-origin request rejected");
    return false;
  }

  /**
   * @brief Read the request body, answering 413 when it exceeds the limit of the endpoint.
   * @return True if the body was read.
   */
  static bool read_body(resp_https_t response, req_https_t request, std::string &out) {
    const auto limit = body_limit_for(request->method, request->path);
    bool too_large = request->content.size() > limit;
    if (auto length = request->header.find("content-length"); !too_large && length != request->header.end()) {
      std::size_t declared = 0;
      const auto &text = length->second;
      auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), declared);
      too_large = ec == std::errc::result_out_of_range || (ec == std::errc {} && declared > limit);
    }
    if (too_large) {
      BOOST_LOG(warning) << "Web UI: request body for ["sv << loggable(request->path) << "] exceeds "sv << limit << " bytes"sv;
      send_error(response, SimpleWeb::StatusCode::client_error_payload_too_large, "Request body too large");
      return false;
    }
    out = request->content.string();
    return true;
  }

  /**
   * @brief Read the request body as JSON, answering 413 or 400 on failure.
   * @return True if `out` holds the parsed document.
   */
  static bool read_json(resp_https_t response, req_https_t request, nlohmann::json &out) {
    std::string body;
    if (!read_body(response, request, body)) {
      return false;
    }
    out = nlohmann::json::parse(body, nullptr, false);
    if (out.is_discarded()) {
      send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "Invalid JSON");
      return false;
    }
    if (exceeds_depth(out, max_json_depth)) {
      send_error(response, SimpleWeb::StatusCode::client_error_bad_request, "JSON is nested too deeply");
      return false;
    }
    return true;
  }

  /**
   * @brief Check if the IP origin is allowed.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if allowed, false otherwise.
   */
  bool checkIPOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      security_log::record(security_log::type::webui_denied, security_log::result::denied, address, security_log::service::webui, {.detail = "address class not allowed"sv});
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }
    return true;
  }

  bool first_setup_allowed(net::net_e origin) {
    return origin == net::PC;
  }

  /**
   * @brief Allow the first credentials to be created only from the host PC.
   * @return True if the request may continue, otherwise a 403 explaining where to set up was sent.
   */
  static bool checkFirstSetupOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (first_setup_allowed(net::from_address(address))) {
      return true;
    }
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- initial setup refused, only the host PC may create the first credentials"sv;
    send_error(response, SimpleWeb::StatusCode::client_error_forbidden, first_setup_local_only_message(net::map_port(PORT_HTTPS)));
    return false;
  }

  /**
   * @brief Authenticate the request.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param needsRedirect Whether to redirect on failure.
   * @return True if authenticated, false otherwise.
   *
   * This function uses session cookies (if set) and ensures they have not expired.
   */
  bool authenticate(resp_https_t response, req_https_t request, bool needsRedirect = false) {
    if (!checkIPOrigin(response, request))
      return false;
    // Changes must come from the Web UI itself, not from another site the browser has open
    if (!check_request_origin(response, request))
      return false;
    // If credentials not set, redirect pages to welcome and refuse API calls.
    if (config::sunshine.username.empty()) {
      if (needsRedirect) {
        send_redirect(response, request, "/welcome");
      } else {
        send_unauthorized(response, request);
      }
      return false;
    }
    // Guard: on failure, redirect if requested.
    auto fg = util::fail_guard([&]() {
      if (needsRedirect) {
        std::string redir_path = "/login?redir=.";
        redir_path += request->path;
        send_redirect(response, request, redir_path.c_str());
      } else {
        send_unauthorized(response, request);
      }
    });
    if (sessionCookie.empty())
      return false;
    // Check for expiry
    if (std::chrono::steady_clock::now() - cookie_creation_time > SESSION_EXPIRE_DURATION) {
      sessionCookie.clear();
      return false;
    }
    auto cookies = request->header.find("cookie");
    if (cookies == request->header.end())
      return false;
    auto authCookie = getCookieValue(cookies->second, "auth");
    if (authCookie.empty()) {
      return false;
    }
    const auto cookieHash = util::hex(crypto::hash(authCookie + config::sunshine.salt)).to_string();
    if (cookieHash.size() != sessionCookie.size() || CRYPTO_memcmp(cookieHash.data(), sessionCookie.data(), cookieHash.size()) != 0) {
      return false;
    }
    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void not_found(resp_https_t response, [[maybe_unused]] req_https_t request) {
    send_error(response, SimpleWeb::StatusCode::client_error_not_found, "Not Found");
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message.
   */
  void bad_request(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Bad Request") {
    send_error(response, SimpleWeb::StatusCode::client_error_bad_request, error_message);
  }

  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The required content type.
   */
  bool validateContentType(resp_https_t response, req_https_t request, const std::string_view& contentType) {
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }

    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    size_t semicolonPos = actualContentType.find(';');
    if (semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;
  }

  namespace {
    constexpr std::size_t max_text_field = 8 * 1024;
    constexpr std::size_t max_apps = 512;

    /**
     * @brief Check an optional string field of an app: type, length and absence of NUL.
     */
    std::optional<std::string> check_text_field(const nlohmann::json &object, const char *key, std::size_t max_size = max_text_field) {
      auto it = object.find(key);
      if (it == object.end()) {
        return std::nullopt;
      }
      if (!it->is_string()) {
        return std::string(key) + " must be a string";
      }
      const auto &text = it->get_ref<const std::string &>();
      if (text.size() > max_size) {
        return std::string(key) + " is too long (max. " + std::to_string(max_size) + " bytes)";
      }
      if (text.find('\0') != std::string::npos) {
        return std::string(key) + " must not contain NUL characters";
      }
      return std::nullopt;
    }

    std::optional<std::string> check_bool_field(const nlohmann::json &object, const char *key) {
      auto it = object.find(key);
      if (it != object.end() && !it->is_boolean()) {
        return std::string(key) + " must be true or false";
      }
      return std::nullopt;
    }

    std::optional<std::string> check_int_field(const nlohmann::json &object, const char *key, std::int64_t min, std::int64_t max) {
      auto it = object.find(key);
      if (it == object.end()) {
        return std::nullopt;
      }
      std::int64_t number = 0;
      if (it->is_number_integer()) {
        number = it->is_number_unsigned() ? static_cast<std::int64_t>(std::min<std::uint64_t>(it->get<std::uint64_t>(), INT64_MAX)) : it->get<std::int64_t>();
      }
      if (!it->is_number_integer() || number < min || number > max) {
        return std::string(key) + " must be a whole number between " + std::to_string(min) + " and " + std::to_string(max);
      }
      return std::nullopt;
    }

    /**
     * @brief Check a list of {do, undo, elevated} entries.
     */
    std::optional<std::string> check_command_list(const nlohmann::json &app, const char *key) {
      auto it = app.find(key);
      if (it == app.end()) {
        return std::nullopt;
      }
      if (!it->is_array() || it->size() > 64) {
        return std::string(key) + " must be a list of at most 64 entries";
      }
      for (const auto &entry : *it) {
        if (!entry.is_object()) {
          return std::string(key) + " entries must be objects";
        }
        for (const char *field : {"do", "undo"}) {
          if (auto error = check_text_field(entry, field)) {
            return std::string(key) + ": " + *error;
          }
        }
        if (auto error = check_bool_field(entry, "elevated")) {
          return std::string(key) + ": " + *error;
        }
      }
      return std::nullopt;
    }

    /// Allowed range of the whole number options of the security log, if `name` is one of them.
    std::optional<std::pair<long, long>> security_int_range(std::string_view name) {
      if (name == "security_log_max_mb") {
        return std::pair<long, long> {1, 100};
      }
      if (name == "ban_threshold") {
        return std::pair<long, long> {3, 1000};
      }
      if (name == "ban_window_seconds") {
        return std::pair<long, long> {10, 86400};
      }
      if (name == "ban_duration_seconds") {
        return std::pair<long, long> {0, 2592000};
      }
      return std::nullopt;
    }

    bool is_known_option(std::string_view name) {
      static const std::set<std::string_view> known {
      "adapter_name", "address_family", "always_send_scancodes", "amd_coder", "amd_enforce_hrd",
      "amd_preanalysis", "amd_quality", "amd_rc", "amd_usage", "amd_vbaq", "audio_sink", "auto_capture_sink",
      "av1_mode", "back_button_timeout", "capture", "cert", "controller", "credentials_file",
      "dd_config_revert_delay", "dd_config_revert_on_disconnect", "dd_configuration_option", "dd_hdr_option",
      "dd_manual_refresh_rate", "dd_manual_resolution", "dd_mode_remapping", "dd_refresh_rate_option",
      "dd_resolution_option", "dd_wa_hdr_toggle_delay", "double_refreshrate", "ds4_back_as_touchpad_click",
      "ds5_inputtino_randomize_mac", "enable_discovery", "enable_input_only_mode", "enable_pairing",
      "encoder", "envvar_compatibility_mode", "external_ip", "fallback_mode", "fec_percentage", "file_apps",
      "file_state", "flags", "forward_rumble", "gamepad", "global_prep_cmd", "global_state_cmd",
      "headless_mode", "hevc_mode", "hide_tray_controls", "high_resolution_scrolling",
      "ignore_encoder_probe_failure", "install_steam_audio_drivers", "isolated_virtual_display_option",
      "keep_sink_default", "key_repeat_delay", "key_repeat_frequency", "key_rightalt_to_key_win",
      "keybindings", "keyboard", "lan_encryption_mode", "legacy_ordering", "limit_framerate", "locale",
      "log_path", "max_bitrate", "min_log_level", "min_threads", "minimum_fps_target", "motion_as_ds4",
      "mouse", "native_pen_touch", "notify_pre_releases", "nvenc_h264_cavlc", "nvenc_intra_refresh",
      "nvenc_latency_over_power", "nvenc_opengl_vulkan_on_dxgi", "nvenc_preset", "nvenc_realtime_hags",
      "nvenc_spatial_aq", "nvenc_twopass", "nvenc_vbv_increase", "origin_web_ui_allowed", "output_name",
      "ping_timeout", "pkey", "port", "qp", "qsv_coder", "qsv_preset", "qsv_slow_hevc", "rtsp_plaintext", "server_cmd",
      "stream_audio", "sunshine_name", "sw_preset", "sw_tune", "system_tray", "touchpad_as_ds4", "upnp",
      "vaapi_strict_rc_buffer", "virtual_display_driver", "virtual_sink", "vt_coder", "vt_realtime",
      "vt_software", "wan_encryption_mode", "ban_duration_seconds", "ban_enabled", "ban_threshold",
      "ban_whitelist", "ban_window_seconds", "security_log", "security_log_max_mb"
      };
      return known.contains(name);
    }
  }  // namespace

  std::optional<std::string> validate_app(const nlohmann::json &app) {
    if (!app.is_object()) {
      return "The application must be a JSON object";
    }
    if (app.size() > 64) {
      return "The application has too many fields";
    }
    if (exceeds_depth(app, 8)) {
      return "The application is nested too deeply";
    }

    auto name = app.find("name");
    if (name == app.end() || !name->is_string()) {
      return "name is required and must be a string";
    }
    auto trimmed = boost::algorithm::trim_copy(name->get_ref<const std::string &>());
    if (trimmed.empty()) {
      return "name must not be empty";
    }
    if (trimmed.size() > 256) {
      return "name is too long (max. 256 bytes)";
    }
    if (std::ranges::any_of(trimmed, [](char ch) {
          return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F;
        })) {
      return "name must not contain control characters";
    }

    if (auto uuid = app.find("uuid"); uuid != app.end()) {
      if (!uuid->is_string()) {
        return "uuid must be a string";
      }
      const auto &text = uuid->get_ref<const std::string &>();
      if (!text.empty() && (text.size() > 64 || !std::ranges::all_of(text, [](char ch) {
                              return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-';
                            }))) {
        return "uuid may only contain letters, digits and '-' (max. 64 characters)";
      }
    }

    for (const char *key : {"cmd", "output", "working-dir", "image-path", "gamepad"}) {
      if (auto error = check_text_field(app, key)) {
        return error;
      }
    }
    if (auto image = app.find("image-path"); image != app.end()) {
      const auto &text = image->get_ref<const std::string &>();
      if (!text.empty() && !boost::iends_with(text, ".png")) {
        return "image-path must be empty or point to a .png file";
      }
    }

    for (const char *key : {"exclude-global-prep-cmd", "exclude-global-state-cmd", "elevated", "auto-detach", "wait-all", "virtual-display", "use-app-identity", "per-client-app-identity", "allow-client-commands", "terminate-on-pause"}) {
      if (auto error = check_bool_field(app, key)) {
        return error;
      }
    }
    if (auto error = check_int_field(app, "exit-timeout", 0, 3600)) {
      return error;
    }
    if (auto error = check_int_field(app, "scale-factor", 20, 500)) {
      return error;
    }

    for (const char *key : {"prep-cmd", "state-cmd"}) {
      if (auto error = check_command_list(app, key)) {
        return error;
      }
    }
    if (auto detached = app.find("detached"); detached != app.end()) {
      if (!detached->is_array() || detached->size() > 64) {
        return "detached must be a list of at most 64 commands";
      }
      for (const auto &command : *detached) {
        if (!command.is_string() || command.get_ref<const std::string &>().size() > max_text_field || command.get_ref<const std::string &>().find('\0') != std::string::npos) {
          return "detached entries must be text of at most 8192 bytes";
        }
      }
    }

    return std::nullopt;
  }

  config_validation_t validate_config_object(const nlohmann::json &input) {
    config_validation_t result;
    if (!input.is_object()) {
      result.error = "The configuration must be a JSON object";
      return result;
    }
    if (input.size() > 512) {
      result.error = "The configuration has too many options";
      return result;
    }
    if (exceeds_depth(input, 8)) {
      result.error = "The configuration is nested too deeply";
      return result;
    }

    for (const auto &[key, value] : input.items()) {
      if (key.empty() || key.size() > 64 || !std::ranges::all_of(key, [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.';
          })) {
        result.error = "Invalid configuration option: " + loggable(key, 64);
        return result;
      }
      if (value.is_null() || (value.is_string() && value.get_ref<const std::string &>().empty())) {
        // Means "use the default"; nothing is stored
        continue;
      }
      // dd_mode_remapping is the only option that is a JSON document
      if (value.is_object() && key != "dd_mode_remapping") {
        result.error = "Option " + key + " must be a text, number, true/false or a list";
        return result;
      }

      // One option per line: a plain value with a line break would smuggle in further options
      // (only JSON array values may span lines, the parser reads them up to the closing bracket)
      std::string text = value.is_string() ? value.get<std::string>() : value.dump();
      const auto first_char = text.find_first_not_of(" \t");
      // A string that looks like a list may span lines only when it is one complete JSON array, so
      // trailing text cannot add further options
      const bool is_list = value.is_structured() ||
                           (first_char != std::string::npos && text[first_char] == '[' && nlohmann::json::accept(text));
      if (text.size() > (is_list ? 64 * 1024 : max_text_field)) {
        result.error = "Option " + key + " is too long";
        return result;
      }
      if (!is_list && text.find_first_of("\r\n") != std::string::npos) {
        result.error = "Option " + key + " must not contain line breaks";
        return result;
      }

      if (key == "port") {
        // The derived ports span port - 5 .. port + 21
        long port = 0;
        const auto trimmed = boost::algorithm::trim_copy(text);
        auto [end, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), port);
        if (ec != std::errc {} || end != trimmed.data() + trimmed.size() || port < 1024 || port > 65535 - 21) {
          result.error = "port must be a number between 1024 and 65514";
          return result;
        }
      }

      if (const auto range = security_int_range(key)) {
        long number = 0;
        const auto trimmed = boost::algorithm::trim_copy(text);
        auto [end, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
        if (ec != std::errc {} || end != trimmed.data() + trimmed.size() || number < range->first || number > range->second) {
          result.error = key + " must be a number between " + std::to_string(range->first) + " and " + std::to_string(range->second);
          return result;
        }
      }
      if (key == "ban_whitelist") {
        std::vector<security_log::cidr_t> entries;
        std::string bad_entry;
        if (!security_log::parse_whitelist(text, entries, bad_entry)) {
          result.error = "ban_whitelist contains an invalid address or range: " + loggable(bad_entry, 64);
          return result;
        }
      }

      if (!is_known_option(key)) {
        result.unknown_keys.push_back(key);
      }
    }
    return result;
  }

  std::string render_config_file(const nlohmann::json &input) {
    std::string text;
    for (const auto &[key, value] : input.items()) {
      if (value.is_null() || (value.is_string() && value.get_ref<const std::string &>().empty())) {
        continue;
      }
      // dump() produces valid JSON, which is not wanted for plain strings in the configuration file
      text += key;
      text += " = ";
      text += value.is_string() ? value.get<std::string>() : value.dump();
      text += '\n';
    }
    return text;
  }

  /**
   * @brief Replace the apps file, answering 500 when it cannot be written.
   */
  static bool store_apps(resp_https_t response, const nlohmann::json &file_tree) {
    if (file_handler::write_file_atomic(config::stream.file_apps.c_str(), file_tree.dump(4)) != 0) {
      BOOST_LOG(error) << "Couldn't write ["sv << config::stream.file_apps << ']';
      send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Could not write the applications file");
      return false;
    }
    return true;
  }

  /**
   * @brief Get the index page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getIndexPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "index.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the PIN page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPinPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "pin.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the apps page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAppsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "apps.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
    response->write(content, headers);
  }

  /**
   * @brief Get the clients page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getClientsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "clients.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the configuration page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfigPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "config.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the password page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPasswordPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "password.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the login page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo Combine this function with getWelcomePage if appropriate.
   */
  void getLoginPage(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "login.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the welcome page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getWelcomePage(resp_https_t response, req_https_t request) {
    // The first credentials can be created here: apply the Web UI origin allow-list
    if (!checkIPOrigin(response, request)) {
      return;
    }

    print_req(request);

    if (!config::sunshine.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    if (!checkFirstSetupOrigin(response, request)) {
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "welcome.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the security page (event log and blocked addresses).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getSecurityPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "security.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the troubleshooting page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getTroubleshootingPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "troubleshooting.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    add_security_headers(headers);
    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getFaviconImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/nyxplay-host.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    add_security_headers(headers, true);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the NyxPlay Host logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getNyxPlayLogoImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-nyxplay-host-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    add_security_headers(headers, true);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    // An empty result means the paths are unrelated (for example another drive)
    return !relPath.empty() && *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    print_req(request);

    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }

    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension (directories and extension-less files have none)
    auto extension = relPath.extension().string();
    auto mimeType = extension.size() > 1 ? mime_types.find(extension.substr(1)) : mime_types.end();
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    add_security_headers(headers, true);
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      file_tree["current_app"] = proc::proc.get_running_app_uuid();
      file_tree["host_uuid"] = http::unique_id;
      file_tree["host_name"] = config::nvhttp.sunshine_name;

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application the UUID must be empty.
   *        To update an existing application, you must provide the current UUID of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file.",
   *   "uuid": "aaaa-bbbb"
   * }
   * @endcode
   *
   * `name` is required (1 to 256 bytes). Text fields are limited to 8 KiB, flags must be booleans,
   * `exit-timeout` is 0 to 3600, `scale-factor` 20 to 500, `image-path` must be empty or a `.png` file,
   * the lists are limited to 64 entries and the file holds at most 512 apps. A definition that breaks
   * these rules is answered with HTTP 400 and the reason; nothing is stored.
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","uuid": "aaaa-bbbb"}}
   */
  void saveApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "application saved"sv);

    // Read the input JSON from the request body.
    nlohmann::json inputTree;
    if (!read_json(response, request, inputTree)) {
      return;
    }

    try {
      if (auto error = validate_app(inputTree)) {
        bad_request(response, request, *error);
        return;
      }
      inputTree["name"] = boost::algorithm::trim_copy(inputTree["name"].get<std::string>());

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);
      if (!fileTree.is_object()) {
        bad_request(response, request, "The applications file is not valid");
        return;
      }

      // Adding to a full list is refused; replacing an entry (same uuid) is not an addition
      if (fileTree.contains("apps") && fileTree["apps"].is_array() && fileTree["apps"].size() >= max_apps) {
        const std::string uuid = inputTree.value("uuid", "");
        const auto &list = fileTree["apps"];
        const bool replaces = !uuid.empty() && std::any_of(list.begin(), list.end(), [&uuid](const nlohmann::json &app) {
          return app.is_object() && app.contains("uuid") && app["uuid"].is_string() && app["uuid"].get<std::string>() == uuid;
        });
        if (!replaces) {
          bad_request(response, request, "Too many applications (max. " + std::to_string(max_apps) + ")");
          return;
        }
      }

      // Migrate/merge the new app into the file tree.
      proc::migrate_apps(&fileTree, &inputTree);

      // Write the updated file tree back to disk.
      if (!store_apps(response, fileTree)) {
        return;
      }
      proc::refresh(config::stream.file_apps);

      // Prepare and send the output response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    security_log::record(security_log::type::cancel, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_name = "Web UI"sv, .app_name = proc::proc.get_last_run_app_name()});
    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Reorder applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/reorder| POST| {"order": ["aaaa-bbbb", "cccc-dddd"]}}
   */
  void reorderApps(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "applications reordered"sv);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      nlohmann::json output_tree;

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);
      if (!fileTree.is_object()) {
        throw std::runtime_error("The applications file is not valid");
      }

      // Get the desired order of UUIDs from the request.
      if (!input_tree.is_object() || !input_tree.contains("order") || !input_tree["order"].is_array()) {
        throw std::runtime_error("Missing or invalid 'order' array in request body");
      }
      const auto& order_uuids_json = input_tree["order"];

      // Get the original apps array from the fileTree.
      // Default to an empty array if "apps" key is missing or if it's present but not an array (after logging an error).
      nlohmann::json original_apps_list = nlohmann::json::array();
      if (fileTree.contains("apps")) {
        if (fileTree["apps"].is_array()) {
          original_apps_list = fileTree["apps"];
        } else {
          // "apps" key exists but is not an array. This is a malformed state.
          BOOST_LOG(error) << "ReorderApps: 'apps' key in apps configuration file ('" << config::stream.file_apps
                           << "') is present but not an array.";
          throw std::runtime_error("'apps' in file is not an array, cannot reorder.");
        }
      } else {
        // "apps" key is missing. Treat as an empty list. Reordering an empty list is valid.
        BOOST_LOG(debug) << "ReorderApps: 'apps' key missing in apps configuration file ('" << config::stream.file_apps
                         << "'). Treating as an empty list for reordering.";
        // original_apps_list is already an empty array, so no specific action needed here.
      }

      nlohmann::json reordered_apps_list = nlohmann::json::array();
      std::vector<bool> item_moved(original_apps_list.size(), false);

      // Phase 1: Place apps according to the 'order' array from the request.
      // Iterate through the desired order of UUIDs.
      for (const auto& uuid_json_value : order_uuids_json) {
        if (!uuid_json_value.is_string()) {
          BOOST_LOG(warning) << "ReorderApps: Encountered a non-string UUID in the 'order' array. Skipping this entry.";
          continue;
        }
        std::string target_uuid = uuid_json_value.get<std::string>();
        bool found_match_for_ordered_uuid = false;

        // Find the first unmoved app in the original list that matches the current target_uuid.
        for (size_t i = 0; i < original_apps_list.size(); ++i) {
          if (item_moved[i]) {
            continue; // This specific app object has already been placed.
          }

          const auto& app_item = original_apps_list[i];
          // Ensure the app item is an object and has a UUID to match against.
          if (app_item.is_object() && app_item.contains("uuid") && app_item["uuid"].is_string()) {
            if (app_item["uuid"].get<std::string>() == target_uuid) {
              reordered_apps_list.push_back(app_item); // Add the found app object to the new list.
              item_moved[i] = true;                    // Mark this specific object as moved.
              found_match_for_ordered_uuid = true;
              break; // Found an app for this UUID, move to the next UUID in the 'order' array.
            }
          }
        }

        if (!found_match_for_ordered_uuid) {
          // This means a UUID specified in the 'order' array was not found in the original_apps_list
          // among the currently available (unmoved) app objects.
          // Per instruction "If the uuid is missing from the original json file, omit it."
          BOOST_LOG(debug) << "ReorderApps: UUID '" << target_uuid << "' from 'order' array not found in available apps list or its matching app was already processed. Omitting.";
        }
      }

      // Phase 2: Append any remaining apps from the original list that were not explicitly ordered.
      // These are app objects that were not marked 'item_moved' in Phase 1.
      for (size_t i = 0; i < original_apps_list.size(); ++i) {
        if (!item_moved[i]) {
          reordered_apps_list.push_back(original_apps_list[i]);
        }
      }

      // Update the fileTree with the new, reordered list of apps.
      fileTree["apps"] = reordered_apps_list;

      // Write the modified fileTree back to the apps configuration file.
      if (!store_apps(response, fileTree)) {
        return;
      }

      // Notify relevant parts of the system that the apps configuration has changed.
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "ReorderApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/delete | POST| { uuid: 'aaaa-bbbb' }}
   */
  void deleteApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "application deleted"sv);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      // Check for required uuid field in body
      if (!input_tree.is_object() || !input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      auto uuid = input_tree["uuid"].get<std::string>();

      // Read the apps file into a nlohmann::json object.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);
      if (!fileTree.is_object()) {
        bad_request(response, request, "The applications file is not valid");
        return;
      }

      // Remove any app with the matching uuid directly from the "apps" array.
      if (fileTree.contains("apps") && fileTree["apps"].is_array()) {
        auto& apps = fileTree["apps"];
        apps.erase(
          std::remove_if(apps.begin(), apps.end(), [&uuid](const nlohmann::json& app) {
            return app.is_object() && app.contains("uuid") && app["uuid"].is_string() && app["uuid"].get<std::string>() == uuid;
          }),
          apps.end()
        );
      }

      // Write the updated JSON back to the file.
      if (!store_apps(response, fileTree)) {
        return;
      }
      proc::refresh(config::stream.file_apps);

      // Prepare and send the response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json named_certs = nvhttp::get_all_clients();
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
#ifdef _WIN32
    output_tree["platform"] = "windows";
#endif
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  bool is_valid_client_display_mode(const std::string &mode) {
    // <width>x<height>[x<refresh rate>] with plain decimal numbers
    static const std::regex pattern {R"(^[0-9]{1,5}x[0-9]{1,5}(x[0-9]+(\.[0-9]+)?)?$)"};
    return mode.empty() || (mode.size() <= 32 && std::regex_match(mode, pattern));
  }

  std::string parse_client_update(const nlohmann::json &input, const crypto::named_cert_t &current, client_update_t &update) {
    if (!input.is_object()) {
      return "Request body must be a JSON object";
    }

    update = {};
    if (!input.contains("uuid") || !input["uuid"].is_string() || input["uuid"].get<std::string>().empty()) {
      return "uuid is required";
    }
    update.uuid = input["uuid"].get<std::string>();

    // Everything else is optional and keeps the stored value when absent
    update.name = current.name;
    update.display_mode = current.display_mode;
    update.do_cmds = current.do_cmds;
    update.undo_cmds = current.undo_cmds;
    update.perm = current.perm;
    update.enable_legacy_ordering = current.enable_legacy_ordering;
    update.allow_client_commands = current.allow_client_commands;
    update.always_use_virtual_display = current.always_use_virtual_display;

    if (input.contains("name")) {
      if (!input["name"].is_string()) {
        return "name must be a string";
      }
      update.name = input["name"].get<std::string>();
      if (!nvhttp::is_valid_pairing_name(update.name)) {
        return "Client name must not be longer than 128 bytes";
      }
    }

    if (input.contains("display_mode")) {
      if (!input["display_mode"].is_string() || !is_valid_client_display_mode(input["display_mode"].get<std::string>())) {
        return "Invalid display mode";
      }
      update.display_mode = input["display_mode"].get<std::string>();
    }

    if (input.contains("perm")) {
      const auto &value = input["perm"];
      const bool usable = value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0);
      if (!usable) {
        return "Invalid permission value";
      }
      const auto raw = value.get<std::uint64_t>();
      if (raw > 0xFFFFFFFFull || (static_cast<std::uint32_t>(raw) & ~static_cast<std::uint32_t>(crypto::PERM::_all)) != 0) {
        return "Invalid permission value";
      }
      update.perm = static_cast<crypto::PERM>(static_cast<std::uint32_t>(raw));
    }

    const std::pair<const char *, bool *> flags[] = {
      {"enable_legacy_ordering", &update.enable_legacy_ordering},
      {"allow_client_commands", &update.allow_client_commands},
      {"always_use_virtual_display", &update.always_use_virtual_display},
    };
    for (const auto &[key, target] : flags) {
      if (input.contains(key)) {
        if (!input[key].is_boolean()) {
          return std::string(key) + " must be a boolean";
        }
        *target = input[key].get<bool>();
      }
    }

    if (input.contains("do")) {
      if (!input["do"].is_array()) {
        return "do must be an array";
      }
      update.do_cmds = nvhttp::extract_command_entries(input, "do");
    }
    if (input.contains("undo")) {
      if (!input["undo"].is_array()) {
        return "undo must be an array";
      }
      update.undo_cmds = nvhttp::extract_command_entries(input, "undo");
    }

    return {};
  }

  /**
   * @brief Update client information.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "name": "<Friendly Name>",
   *   "display_mode": "1920x1080x59.94",
   *   "do": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "undo": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "perm": <uint32_t>
   * }
   * @endcode
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      nlohmann::json output_tree;

      // Fields that are not part of the request keep their stored value
      std::string uuid;
      if (input_tree.is_object() && input_tree.contains("uuid") && input_tree["uuid"].is_string()) {
        uuid = input_tree["uuid"].get<std::string>();
      }
      auto current = nvhttp::get_client(uuid);
      client_update_t update;
      if (current) {
        if (auto error = parse_client_update(input_tree, *current, update); !error.empty()) {
          bad_request(response, request, error);
          return;
        }
      } else {
        // Reports a missing uuid; an unknown one is answered below
        crypto::named_cert_t none {};
        if (auto error = parse_client_update(input_tree, none, update); !error.empty()) {
          bad_request(response, request, error);
          return;
        }
      }

      output_tree["status"] = current && nvhttp::update_device_info(
        update.uuid,
        update.name,
        update.display_mode,
        update.do_cmds,
        update.undo_cmds,
        update.perm,
        update.enable_legacy_ordering,
        update.allow_client_commands,
        update.always_use_virtual_display
      );
      if (output_tree["status"].get<bool>()) {
        security_log::record(security_log::type::client_update, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_uuid = update.uuid, .client_name = update.name, .detail = "client settings changed in the Web UI"sv});
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/clients/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      nlohmann::json output_tree;
      std::string uuid = input_tree.is_object() ? input_tree.value("uuid", "") : std::string {};
      const auto unpaired = nvhttp::get_client(uuid);
      output_tree["status"] = nvhttp::unpair_client(uuid);
      if (unpaired && output_tree["status"].get<bool>()) {
        security_log::record(security_log::type::unpair, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_uuid = unpaired->uuid, .client_name = unpaired->name, .detail = "unpaired in the Web UI"sv});
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    security_log::record(security_log::type::unpair, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_name = "Web UI"sv, .detail = "all clients unpaired"sv});
    nvhttp::erase_all_clients();
    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
#ifdef _WIN32
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#endif
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      output_tree[name] = value;
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * The call replaces the whole configuration file: send the complete set of options that differ from
   * the defaults (the Web UI does this). `null` and empty values are not stored. The previous file is
   * kept as `<config file>.bak`. Options the host does not know are stored too and listed in the
   * response under `unknown_options`.
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "configuration saved"sv);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      const auto validation = validate_config_object(input_tree);
      if (!validation.error.empty()) {
        bad_request(response, request, validation.error);
        return;
      }
      for (const auto &key : validation.unknown_keys) {
        BOOST_LOG(warning) << "SaveConfig: unknown option ["sv << key << "] is stored as is"sv;
      }

      // Keep the previous file next to the new one, then replace it in one step
      const auto &config_file = config::sunshine.config_file;
      std::error_code ec;
      if (fs::exists(config_file, ec)) {
        fs::copy_file(config_file, config_file + ".bak", fs::copy_options::overwrite_existing, ec);
        if (ec) {
          BOOST_LOG(warning) << "SaveConfig: couldn't create the backup: "sv << ec.message();
        }
      }
      if (file_handler::write_file_atomic(config_file.c_str(), render_config_file(input_tree)) != 0) {
        BOOST_LOG(error) << "SaveConfig: couldn't write ["sv << config_file << ']';
        send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Could not write the configuration");
        return;
      }

      nlohmann::json output_tree;
      output_tree["status"] = true;
      if (!validation.unknown_keys.empty()) {
        output_tree["unknown_options"] = validation.unknown_keys;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      if (!input_tree.is_object()) {
        bad_request(response, request, "Request body must be a JSON object");
        return;
      }
      nlohmann::json output_tree;
      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      if (key.size() > 64 || !std::ranges::all_of(key, [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
          })) {
        bad_request(response, request, "Cover key may only contain letters, digits, '_' and '-' (max. 64 characters)");
        return;
      }
      std::string url = input_tree.value("url", "");
      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string path = coverdir + http::url_escape(key) + ".png";
      // Covers are served to clients as PNG: only store PNG data of a sane size
      constexpr std::string_view png_magic {"\x89PNG\r\n\x1a\n", 8};
      constexpr std::size_t max_cover_size = 8u * 1024 * 1024;
      if (!url.empty()) {
        if (!boost::istarts_with(url, "https://") || http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        // The Web UI has a single worker thread: do not wait for a slow server for long
        if (!http::download_file(url, path, CURL_SSLVERSION_TLSv1_2, 20)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        std::array<char, 8> head {};
        if (!ec) {
          std::ifstream in(path, std::ios::binary);
          in.read(head.data(), head.size());
        }
        if (ec || size < png_magic.size() || size > max_cover_size || std::string_view {head.data(), head.size()} != png_magic) {
          fs::remove(path, ec);
          bad_request(response, request, "The downloaded file is not a PNG image (max. 8 MB)");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));
        if (data.size() < png_magic.size() || data.size() > max_cover_size || std::string_view {data}.substr(0, png_magic.size()) != png_magic) {
          bad_request(response, request, "Cover must be a PNG image (max. 8 MB)");
          return;
        }
        // Binary mode: text mode corrupts the image on Windows
        std::ofstream imgfile(path, std::ios::binary);
        imgfile.write(data.data(), static_cast<std::streamsize>(data.size()));
        imgfile.flush();
        if (!imgfile.good()) {
          std::error_code ec;
          imgfile.close();
          fs::remove(path, ec);
          send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Could not store the cover");
          return;
        }
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    // Only the end of the log is of interest and it can grow very large
    constexpr std::streamoff max_log_bytes = 2 * 1024 * 1024;
    std::string content;
    {
      std::ifstream in(config::sunshine.log_file, std::ios::binary | std::ios::ate);
      if (in) {
        const std::streamoff size = in.tellg();
        const std::streamoff start = size > max_log_bytes ? size - max_log_bytes : 0;
        in.seekg(start);
        content.resize(static_cast<std::size_t>(size - start));
        in.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
        if (start > 0) {
          // Start with a complete line
          if (auto newline = content.find('\n'); newline != std::string::npos) {
            content.erase(0, newline + 1);
          }
        }
        boost::erase_all(content, "\r");
      }
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    add_security_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(resp_https_t response, req_https_t request) {
    // While no credentials exist this endpoint is unauthenticated: the origin allow-list and the
    // same-origin rule still apply, and only the host PC itself may create the first credentials
    if (config::sunshine.username.empty() &&
        (!checkIPOrigin(response, request) || !checkFirstSetupOrigin(response, request) || !check_request_origin(response, request))) {
      return;
    }
    if ((!config::sunshine.username.empty() && !authenticate(response, request)) || !validateContentType(response, request, "application/json"))
      return;
    print_req(request);
    log_admin(request, "Web UI credentials changed"sv);
    std::vector<std::string> errors;
    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }
    try {
      if (!input_tree.is_object()) {
        bad_request(response, request, "Request body must be a JSON object");
        return;
      }
      nlohmann::json output_tree;
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty())
        newUsername = username;
      if (newUsername.empty() || newUsername.size() > 64) {
        errors.push_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() ||
            (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword)
            errors.push_back("Password Mismatch");
          else if (newPassword.size() < MIN_PASSWORD_LENGTH)
            errors.push_back("Password Too Short");
          else {
            // The old credentials and the session stay valid unless the new ones were stored
            if (http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword) != 0 ||
                http::reload_user_creds(config::sunshine.credentials_file) != 0) {
              send_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "Could not save the credentials");
              return;
            }
            sessionCookie.clear();  // force re-login
            output_tree["status"] = true;
          }
        } else {
          errors.push_back("Invalid Current Credentials");
        }
      }
      if (!errors.empty()) {
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(),
                                              [](const std::string &a, const std::string &b) {
                                                return a.empty() ? b : a + ", " + b;
                                              });
        bad_request(response, request, error);
        return;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get a one-time password (OTP).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/otp| GET| null}
   */
  void getOTP(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "one-time PIN created"sv);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    nlohmann::json output_tree;
    try {
      if (!input_tree.is_object()) {
        throw std::runtime_error("Request body must be a JSON object");
      }

      std::string passphrase = input_tree.value("passphrase", "");
      if (passphrase.empty())
        throw std::runtime_error("Passphrase not provided!");
      if (passphrase.size() < 4)
        throw std::runtime_error("Passphrase too short!");

      std::string deviceName = input_tree.value("deviceName", "");
      output_tree["otp"] = nvhttp::request_otp(passphrase, deviceName);
      output_tree["ip"] = platf::get_local_ip_for_gateway();
      output_tree["name"] = config::nvhttp.sunshine_name;
      output_tree["status"] = true;
      output_tree["message"] = "OTP created, effective within 3 minutes.";
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "OTP creation failed: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Pending pairing requests as JSON.
   */
  nlohmann::json pending_pairings_json(const std::vector<nvhttp::pending_pairing_t> &pairings) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto &pairing : pairings) {
      list.push_back({
        {"id", pairing.id},
        {"name", pairing.name},
        {"address", pairing.address},
      });
    }
    return list;
  }

  /**
   * @brief List client pairing requests that are waiting for PIN approval.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/pin| GET| null}
   */
  void getPendingPairings(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["pairings"] = pending_pairings_json(nvhttp::get_pending_pairings());
    send_response(response, output_tree);
  }

  /**
   * @brief Cancel a client pairing request that is waiting for PIN approval.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the DELETE request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pairing_id": "<pairing_id>"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| DELETE| {"pairing_id":"0123456789abcdef0123456789abcdef"}}
   */
  void cancelPairing(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      const std::string pairing_id = input_tree.is_object() ? input_tree.value("pairing_id", "") : std::string {};
      if (!nvhttp::is_valid_pairing_id(pairing_id)) {
        bad_request(response, request, "pairing_id must contain exactly 32 hexadecimal characters");
        return;
      }

      nlohmann::json output_tree;
      output_tree["status"] = nvhttp::cancel_pairing(pairing_id);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "CancelPairing: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a PIN code to a pairing request.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pairing_id": "<pairing_id>",
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * `pairing_id` (from `GET /api/pin`) selects the request and is required: without it nothing is
   * paired and the call is answered with HTTP 400, so a PIN can never be handed to a request the
   * operator did not choose.
   *
   * @api_examples{/api/pin| POST| {"pairing_id":"0123456789abcdef0123456789abcdef","pin":"1234","name":"My PC"}}
   */
  void savePin(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "pairing PIN entered"sv);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      if (!input_tree.is_object()) {
        bad_request(response, request, "Request body must be a JSON object");
        return;
      }
      nlohmann::json output_tree;
      std::string pin = input_tree.value("pin", "");
      std::string name = input_tree.value("name", "");
      std::string pairing_id = input_tree.value("pairing_id", "");

      if (!nvhttp::is_valid_pairing_pin(pin)) {
        bad_request(response, request, "PIN must contain exactly 4 numeric digits");
        return;
      }
      if (!nvhttp::is_valid_pairing_name(name)) {
        bad_request(response, request, "Client name must not be longer than 128 bytes");
        return;
      }

      if (pairing_id.empty()) {
        // A PIN is only handed to a request the operator selected, never to whichever one is waiting
        bad_request(response, request, "pairing_id is required: select the device that is waiting for the PIN (GET /api/pin lists the waiting devices)");
        return;
      } else if (!nvhttp::is_valid_pairing_id(pairing_id)) {
        bad_request(response, request, "pairing_id must contain exactly 32 hexadecimal characters");
        return;
      }

      output_tree["status"] = nvhttp::pin(pairing_id, pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "display device persistence reset"sv);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Restart NyxPlay Host.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "host restart requested"sv);

    proc::proc.terminate();

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief UTF-8 text of a path (JSON requires UTF-8 regardless of the system code page).
   */
  std::string path_to_utf8(const fs::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
  }

  /**
   * @brief Describe an import source as JSON.
   */
  nlohmann::json import_source_json(const config_import::source_t &source) {
    return {
      {"product", source.product},
      {"path", path_to_utf8(source.config_dir)},
      {"has_settings", source.has_settings},
      {"has_apps", source.has_apps},
      {"has_state", source.has_state},
      {"has_identity", source.has_identity},
    };
  }

  /**
   * @brief List installed Apollo / Sunshine hosts whose configuration can be imported.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/import| GET| null}
   */
  void getImportSources(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const auto target_dir = fs::path(config::sunshine.config_file).parent_path();
    nlohmann::json output_tree;
    output_tree["sources"] = nlohmann::json::array();
    for (const auto &source : config_import::detect_sources(target_dir)) {
      output_tree["sources"].push_back(import_source_json(source));
    }
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Import the configuration of another host and restart.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "path": "C:/Program Files/Apollo/config",
   *   "settings": true,
   *   "apps": true,
   *   "clients": true,
   *   "web_credentials": false,
   *   "keep_port": true
   * }
   * @endcode
   *
   * @api_examples{/api/import| POST| {"path":"C:/Program Files/Apollo/config","settings":true,"apps":true,"clients":true}}
   */
  void importConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "configuration import"sv);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      if (!input_tree.is_object()) {
        bad_request(response, request, "Request body must be a JSON object");
        return;
      }

      const std::string path = input_tree.value("path", "");
      if (path.empty() || path.size() > 1024) {
        bad_request(response, request, "path is required");
        return;
      }
      // Probing a network path makes the host authenticate against a remote server. Device prefixes
      // (\\?\UNC\..., \??\UNC\...) reach the same shares.
      auto normalized = boost::algorithm::to_lower_copy(path);
      std::ranges::replace(normalized, '\\', '/');
      if (normalized.starts_with("//") || normalized.starts_with("/?" "?/") || normalized.find("/unc/") != std::string::npos) {
        bad_request(response, request, "Network paths are not supported");
        return;
      }

      config_import::options_t options;
      options.settings = input_tree.value("settings", true);
      options.apps = input_tree.value("apps", true);
      options.clients = input_tree.value("clients", true);
      options.web_credentials = input_tree.value("web_credentials", false);
      options.keep_port = input_tree.value("keep_port", true);

      const auto target_dir = fs::path(config::sunshine.config_file).parent_path();
      const auto result = config_import::import_from(fs::path(std::u8string(path.begin(), path.end())), target_dir, options);

      nlohmann::json output_tree;
      output_tree["status"] = result.ok;
      output_tree["imported"] = result.imported;
      output_tree["warnings"] = result.warnings;
      if (!result.ok) {
        output_tree["error"] = result.error;
        BOOST_LOG(warning) << "Import from ["sv << path << "] failed: "sv << result.error;
      } else {
        output_tree["backup"] = path_to_utf8(result.backup_dir);
        output_tree["restarting"] = true;
        BOOST_LOG(info) << "Imported the configuration from ["sv << path << "], restarting"sv;
      }
      send_response(response, output_tree);

      if (result.ok) {
        // The running host still holds the old settings and client list in memory (and would write
        // them back): restart right after the response was delivered.
        std::thread([]() {
          std::this_thread::sleep_for(1s);
          proc::proc.terminate();
          platf::restart();
        }).detach();
      }
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "ImportConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Quit NyxPlay Host.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * On Windows, if running in a service, a special shutdown code is returned.
   */
  void quit(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);
    log_admin(request, "host shutdown requested"sv);

    BOOST_LOG(warning) << "Requested quit from config page!"sv;

    proc::proc.terminate();

#ifdef _WIN32
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
    } else
#endif
    {
      lifetime::exit_sunshine(0, true);
    }
    // If exit fails, write a response after 5 seconds.
    std::thread write_resp([response]{
      std::this_thread::sleep_for(5s);
      response->write();
    });
    write_resp.detach();
  }

  /**
   * @brief Launch an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void launchApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      // Check for required uuid field in body
      if (!input_tree.is_object() || !input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      std::string uuid = input_tree["uuid"].get<std::string>();

      nlohmann::json output_tree;
      const auto &apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          crypto::named_cert_t named_cert {
            .name = "",
            .uuid = http::unique_id,
            .perm = crypto::PERM::_all,
          };
          BOOST_LOG(info) << "Launching app ["sv << app.name << "] from web UI"sv;
          auto launch_session = nvhttp::make_launch_session(true, false, request->parse_query_string(), &named_cert);
          if (!launch_session) {
            bad_request(response, request, "Invalid display mode");
            return;
          }
          auto err = proc::proc.execute(app, launch_session);
          if (err) {
            bad_request(response, request, err == 503 ?
                        "Failed to initialize video capture/encoding. Is a display connected and turned on?" :
                        "Failed to start the specified application");
          } else {
            output_tree["status"] = true;
            security_log::record(security_log::type::launch, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_name = "Web UI"sv, .app_id = app.uuid, .app_name = app.name});
            send_response(response, output_tree);
          }
          return;
        }
      }
      BOOST_LOG(error) << "Couldn't find app with uuid ["sv << uuid << ']';
      bad_request(response, request, "Cannot find requested application");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "LaunchApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Disconnect a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void disconnect(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    try {
      nlohmann::json output_tree;
      std::string uuid = input_tree.is_object() ? input_tree.value("uuid", "") : std::string {};
      output_tree["status"] = nvhttp::find_and_stop_session(uuid, true);
      if (output_tree["status"].get<bool>()) {
        const auto stopped = nvhttp::get_client(uuid);
        security_log::record(security_log::type::admin_action, security_log::result::ok, net::addr_to_normalized_string(request->remote_endpoint().address()), security_log::service::webui, {.client_uuid = uuid, .client_name = stopped ? std::string_view {stopped->name} : std::string_view {}, .detail = "client disconnected in the Web UI"sv});
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Disconnect: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Login the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "username": "<username>",
   *   "password": "<password>"
   * }
   * @endcode
   */
  void login(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request) || !check_request_origin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    // Slow down password guessing: after a failed login further attempts from the same address are
    // refused for an exponentially growing delay (1 s, 2 s, 4 s ... capped at 60 s)
    struct login_throttle_t {
      int failures = 0;
      std::chrono::steady_clock::time_point next_allowed {};
    };
    static std::map<std::string, login_throttle_t> login_throttle;

    const auto now = std::chrono::steady_clock::now();
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    std::erase_if(login_throttle, [&](const auto &entry) {
      return now - entry.second.next_allowed > 10min;
    });
    if (auto throttle = login_throttle.find(address); throttle != login_throttle.end() && now < throttle->second.next_allowed) {
      BOOST_LOG(warning) << "Web UI: ["sv << address << "] -- login throttled"sv;
      security_log::record(security_log::type::webui_denied, security_log::result::denied, address, security_log::service::webui, {.detail = "login throttled"sv});
      const auto wait = std::max<long long>(1, std::chrono::ceil<std::chrono::seconds>(throttle->second.next_allowed - now).count());
      send_error(response, SimpleWeb::StatusCode::client_error_too_many_requests, "Too many login attempts", {{"Retry-After", std::to_string(wait)}});
      return;
    }

    // Anything that does not end in a session counts as a failed attempt
    std::string_view failure_detail = "invalid request"sv;
    auto record_failure = [&]{
      security_log::record(security_log::type::webui_login_failure, security_log::result::fail, address, security_log::service::webui, {.detail = failure_detail});
      if (login_throttle.size() >= 1024 && !login_throttle.contains(address)) {
        // Make room for the new address by dropping the entry that is released first
        auto oldest = std::min_element(login_throttle.begin(), login_throttle.end(), [](const auto &a, const auto &b) {
          return a.second.next_allowed < b.second.next_allowed;
        });
        login_throttle.erase(oldest);
      }
      {
        auto &throttle = login_throttle[address];
        throttle.failures = std::min(throttle.failures + 1, 1000);
        throttle.next_allowed = std::chrono::steady_clock::now() + std::chrono::seconds(login_delay_seconds(throttle.failures));
      }
    };

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      record_failure();
      return;
    }

    auto fg = util::fail_guard([&]{
      record_failure();
      send_error(response, SimpleWeb::StatusCode::client_error_unauthorized, "Unauthorized");
    });

    try {
      if (!input_tree.is_object()) {
        return;
      }
      std::string username = input_tree.value("username", "");
      std::string password = input_tree.value("password", "");
      std::string hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
      const bool password_ok = hash.size() == config::sunshine.password.size() &&
                               CRYPTO_memcmp(hash.data(), config::sunshine.password.data(), hash.size()) == 0;
      if (!boost::iequals(username, config::sunshine.username) || !password_ok) {
        BOOST_LOG(warning) << "Web UI: ["sv << address << "] -- login failed"sv;
        failure_detail = "wrong user name or password"sv;
        return;
      }
      login_throttle.erase(address);
      security_log::record(security_log::type::webui_login_success, security_log::result::ok, address, security_log::service::webui, {.client_name = config::sunshine.username});
      std::string sessionCookieRaw = crypto::rand_alphabet(64);
      sessionCookie = util::hex(crypto::hash(sessionCookieRaw + config::sunshine.salt)).to_string();
      cookie_creation_time = std::chrono::steady_clock::now();
      // HttpOnly: scripts never need the session token. Max-Age matches SESSION_EXPIRE_DURATION.
      SimpleWeb::CaseInsensitiveMultimap headers {
        { "Set-Cookie", "auth=" + sessionCookieRaw + "; Secure; HttpOnly; SameSite=Strict; Max-Age=" +
                          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(SESSION_EXPIRE_DURATION).count()) + "; Path=/" }
      };
      add_security_headers(headers);
      response->write(headers);
      fg.disable();
    } catch (std::exception &e) {
      // Wrong types in the body (a number as the user name, ...) are a failed attempt
      BOOST_LOG(warning) << "Web UI Login failed: ["sv << address << "]: "sv << e.what();
    }
  }

  /**
   * @brief Get the security events.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Query: `type` (comma separated), `ip`, `client`, `app`, `result`, `since`, `until` (ISO-8601), `limit` (1..1000, default 200), `offset`.
   *
   * @api_examples{/api/security/events?type=launch,resume&limit=50| GET| null}
   */
  void getSecurityEvents(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::map<std::string, std::string> query;
    for (const auto &[name, value] : request->parse_query_string()) {
      query.emplace(boost::algorithm::to_lower_copy(name), value);
    }
    std::string error;
    auto filter = security_log::parse_filter(query, error);
    if (!filter) {
      bad_request(response, request, error);
      return;
    }
    send_response(response, security_log::events_json(*filter));
  }

  /**
   * @brief Get the security statistics.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Query: `hours` (1..720, default 24).
   *
   * @api_examples{/api/security/stats?hours=24| GET| null}
   */
  void getSecurityStats(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::map<std::string, std::string> query;
    for (const auto &[name, value] : request->parse_query_string()) {
      query.emplace(boost::algorithm::to_lower_copy(name), value);
    }
    std::string error;
    auto hours = security_log::parse_hours(query, error);
    if (!hours) {
      bad_request(response, request, error);
      return;
    }
    send_response(response, security_log::stats_json(*hours));
  }

  /**
   * @brief Get the blocked addresses.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/security/bans| GET| null}
   */
  void getSecurityBans(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    send_response(response, security_log::bans_json());
  }

  /**
   * @brief Lift the block of an address.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/security/bans/unban| POST| {"ip":"203.0.113.9"}}
   */
  void unbanAddress(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    if (!input_tree.is_object() || !input_tree.contains("ip") || !input_tree["ip"].is_string() || input_tree["ip"].get_ref<const std::string &>().size() > 64) {
      bad_request(response, request, "ip must be an IP address");
      return;
    }
    const auto error = security_log::unban(input_tree["ip"].get_ref<const std::string &>(), net::addr_to_normalized_string(request->remote_endpoint().address()));
    if (!error.empty()) {
      bad_request(response, request, error);
      return;
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Block an address by hand.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * `duration_seconds` is optional (default: ban_duration_seconds), 0 blocks until the block is lifted.
   *
   * @api_examples{/api/security/bans/add| POST| {"ip":"203.0.113.9","duration_seconds":3600,"reason":"scanner"}}
   */
  void banAddress(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json input_tree;
    if (!read_json(response, request, input_tree)) {
      return;
    }

    if (!input_tree.is_object() || !input_tree.contains("ip") || !input_tree["ip"].is_string() || input_tree["ip"].get_ref<const std::string &>().size() > 64) {
      bad_request(response, request, "ip must be an IP address");
      return;
    }
    if (auto error = check_int_field(input_tree, "duration_seconds", 0, 2592000)) {
      bad_request(response, request, *error);
      return;
    }
    if (auto error = check_text_field(input_tree, "reason", 120)) {
      bad_request(response, request, *error);
      return;
    }
    const long long duration = input_tree.contains("duration_seconds") ? input_tree["duration_seconds"].get<long long>() : -1;
    const std::string reason = input_tree.contains("reason") ? input_tree["reason"].get<std::string>() : std::string {};

    const auto error = security_log::ban(input_tree["ip"].get_ref<const std::string &>(), duration, reason.empty() ? "blocked by an administrator"sv : std::string_view {reason}, net::addr_to_normalized_string(request->remote_endpoint().address()));
    if (!error.empty()) {
      bad_request(response, request, error);
      return;
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Delete the recorded security events (memory and files).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/security/events/clear| POST| null}
   */
  void clearSecurityEvents(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    security_log::clear_events(net::addr_to_normalized_string(request->remote_endpoint().address()));
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Start the HTTPS server.
   */
  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    server.default_resource["DELETE"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PATCH"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["POST"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PUT"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["GET"] = not_found;
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/login/?$"]["GET"] = getLoginPage;
    server.resource["^/security/?$"]["GET"] = getSecurityPage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/api/login"]["POST"] = login;
    server.resource["^/api/pin$"]["GET"] = getPendingPairings;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/pin$"]["DELETE"] = cancelPairing;
    server.resource["^/api/otp$"]["POST"] = getOTP;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/reorder$"]["POST"] = reorderApps;
    server.resource["^/api/apps/delete$"]["POST"] = deleteApp;
    server.resource["^/api/apps/launch$"]["POST"] = launchApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/import$"]["GET"] = getImportSources;
    server.resource["^/api/import$"]["POST"] = importConfig;
    server.resource["^/api/quit$"]["POST"] = quit;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/disconnect$"]["POST"] = disconnect;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/security/events$"]["GET"] = getSecurityEvents;
    server.resource["^/api/security/events/clear$"]["POST"] = clearSecurityEvents;
    server.resource["^/api/security/stats$"]["GET"] = getSecurityStats;
    server.resource["^/api/security/bans$"]["GET"] = getSecurityBans;
    server.resource["^/api/security/bans/unban$"]["POST"] = unbanAddress;
    server.resource["^/api/security/bans/add$"]["POST"] = banAddress;
    server.resource["^/images/nyxplay-host.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-nyxplay-host-45.png$"]["GET"] = getNyxPlayLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.config.reuse_address = true;
    server.config.address = net::af_to_any_address_string(address_family);
    server.config.port = port_https;
    // Requests are buffered before authentication: bound their size (cover images are uploaded as base64).
    // The handlers apply tighter limits per endpoint (see body_limit_for).
    server.config.max_request_streambuf_size = MAX_REQUEST_SIZE;
    server.config.timeout_content = 60;

    auto accept_and_run = [&](auto *server) {
      try {
        server->start([port_https](unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
