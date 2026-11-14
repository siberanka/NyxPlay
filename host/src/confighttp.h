/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <string>

// local includes
#include "nvhttp.h"
#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  constexpr auto SESSION_EXPIRE_DURATION = 24h * 15;
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
