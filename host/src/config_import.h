/**
 * @file src/config_import.h
 * @brief Declarations for importing the configuration of another host (Apollo / Sunshine).
 *
 * The importer only ever reads the source directory. Everything written to the target directory
 * is preceded by a backup of the file it replaces.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Migration of settings, applications, paired clients and web credentials from another host.
 */
namespace config_import {
  /**
   * @brief Description of a configuration directory that can be imported.
   */
  struct source_t {
    std::string product;  ///< "Apollo", "Sunshine" or "Custom".
    std::filesystem::path config_dir;  ///< Directory containing sunshine.conf.
    bool has_settings = false;  ///< sunshine.conf is present.
    bool has_apps = false;  ///< The apps file is present.
    bool has_state = false;  ///< The state file is present (paired clients / web credentials).
    bool has_identity = false;  ///< Host certificate and private key are present.
  };

  /**
   * @brief What to import.
   */
  struct options_t {
    bool settings = true;  ///< sunshine.conf
    bool apps = true;  ///< Application list (and its covers).
    bool clients = true;  ///< Paired clients and host identity (unique id, certificate, private key).
    bool web_credentials = false;  ///< Web UI username and salted password hash.
    bool keep_port = true;  ///< false drops the "port" setting so the default port is used.
  };

  /**
   * @brief Outcome of an import.
   */
  struct result_t {
    bool ok = false;
    std::vector<std::string> imported;  ///< Human readable list of imported parts.
    std::vector<std::string> warnings;  ///< Things the user should review.
    std::string error;  ///< Set when !ok.
    std::filesystem::path backup_dir;  ///< Empty when nothing had to be backed up.
  };

  /**
   * @brief Find well-known configuration directories of other hosts on this system.
   * @param exclude The directory of this host. It is never returned.
   * @return Existing directories that contain sunshine.conf, an apps file or a state file.
   */
  std::vector<source_t> detect_sources(const std::filesystem::path &exclude = {});

  /**
   * @brief Describe a directory chosen by the user.
   * @param config_dir Directory that should contain sunshine.conf.
   * @return The description, or nullopt if the directory does not exist or holds nothing importable.
   */
  std::optional<source_t> inspect_source(const std::filesystem::path &config_dir);

  /**
   * @brief Import from another host's config directory into this host's config directory.
   * @param source_config_dir Directory of the other host. Never modified.
   * @param target_config_dir Directory of this host.
   * @param options Parts to import.
   * @return The result. This function does not throw.
   */
  result_t import_from(const std::filesystem::path &source_config_dir, const std::filesystem::path &target_config_dir, const options_t &options = {});
}  // namespace config_import
