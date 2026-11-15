/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "network.h"
#include "nvhttp.h"
#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  constexpr auto SESSION_EXPIRE_DURATION = 24h * 15;

  /// Upper bound for any request (headers and body) accepted by the Web UI server.
  constexpr std::size_t MAX_REQUEST_SIZE = 12 * 1024 * 1024;
  /// Largest body of a cover upload (an 8 MiB PNG as base64 plus the JSON envelope).
  constexpr std::size_t MAX_COVER_REQUEST_SIZE = 11 * 1024 * 1024;
  /// Shortest password the Web UI accepts.
  constexpr std::size_t MIN_PASSWORD_LENGTH = 8;
  void start();

  /**
   * @brief Validated fields of a client update request.
   */
  struct client_update_t {
    std::string uuid;
    std::string name;
    std::string display_mode;
    nvhttp::cmd_list_t do_cmds;
    nvhttp::cmd_list_t undo_cmds;
    crypto::PERM perm = crypto::PERM::_no;
    bool enable_legacy_ordering = true;
    bool allow_client_commands = true;
    bool always_use_virtual_display = false;
  };

  /**
   * @brief Validate a client update request and merge it with the stored client.
   * @param input The request body.
   * @param current The stored client; fields missing from the request keep these values.
   * @param update Receives the values to store.
   * @return An empty string on success, otherwise the reason the request was rejected.
   */
  std::string parse_client_update(const nlohmann::json &input, const crypto::named_cert_t &current, client_update_t &update);

  /**
   * @brief Decide whether a request comes from the Web UI's own origin.
   * @param method The HTTP method. Safe methods (GET, HEAD, OPTIONS) are never restricted.
   * @param host The value of the `Host` header.
   * @param origin The value of the `Origin` header, empty when absent.
   * @param referer The value of the `Referer` header, empty when absent.
   * @param sec_fetch_site The value of the `Sec-Fetch-Site` header, empty when absent.
   * @return `true` when the request may change state.
   *
   * Browsers always send `Sec-Fetch-Site` or `Origin` on cross-site requests. Requests that carry
   * none of the headers (scripts, curl) are accepted: they still need the session cookie.
   */
  bool is_same_origin(std::string_view method, std::string_view host, std::string_view origin, std::string_view referer, std::string_view sec_fetch_site);

  /**
   * @brief Message returned when the first Web UI credentials are requested from another machine.
   * @param web_ui_port The configured Web UI port.
   */
  inline std::string first_setup_local_only_message(unsigned short web_ui_port) {
    return "Initial setup is only allowed from the host PC. Open the Web UI on the PC that runs NyxPlay Host (https://localhost:" +
           std::to_string(web_ui_port) + ") to create the credentials.";
  }

  /**
   * @brief Decide whether the first Web UI credentials may be created from a client address.
   * @param origin The network class of the client address.
   * @return `true` only for loopback clients. Once credentials exist the normal login applies instead.
   *
   * Until credentials exist anyone who can reach the Web UI could claim it, so the first account
   * can only be created on the host PC itself.
   */
  bool first_setup_allowed(net::net_e origin);

  /**
   * @brief Validate an application definition submitted to `POST /api/apps`.
   * @param app The application as JSON.
   * @return `std::nullopt` when the definition is usable, otherwise the reason it was rejected.
   */
  std::optional<std::string> validate_app(const nlohmann::json &app);

  /**
   * @brief Result of validating a configuration document.
   */
  struct config_validation_t {
    std::string error;                      ///< Empty when the document is valid.
    std::vector<std::string> unknown_keys;  ///< Valid keys that are not known options (kept, reported).
  };

  /**
   * @brief Validate the document submitted to `POST /api/config`.
   * @param input The document. It replaces the whole configuration file.
   */
  config_validation_t validate_config_object(const nlohmann::json &input);

  /**
   * @brief Render a validated configuration document as the text of the configuration file.
   *
   * Entries that are `null` or an empty string are left out (they mean "use the default").
   */
  std::string render_config_file(const nlohmann::json &input);

  /**
   * @brief Maximum accepted request body for an endpoint.
   * @param method The HTTP method.
   * @param path The request path.
   * @return The limit in bytes.
   */
  std::size_t body_limit_for(std::string_view method, std::string_view path);

  /**
   * @brief Delay before the next login attempt is accepted.
   * @param failures The number of consecutive failed attempts.
   * @return Seconds, 0 when there is no delay. Grows exponentially up to 60.
   */
  int login_delay_seconds(int failures);

  /**
   * @brief Retrieve the value of a cookie from a `Cookie` header.
   * @return The value, an empty string when the cookie is absent.
   */
  std::string getCookieValue(const std::string &cookieString, const std::string &key);
}  // namespace confighttp

// mime types map
const std::map<std::string, std::string> mime_types = {
  {"css", "text/css"},
  {"gif", "image/gif"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/javascript"},
  {"json", "application/json"},
  {"png", "image/png"},
  {"svg", "image/svg+xml"},
  {"ttf", "font/ttf"},
  {"txt", "text/plain"},
  {"woff2", "font/woff2"},
  {"xml", "text/xml"},
};
