/**
 * @file src/update_install.h
 * @brief One-click installation of a new NyxPlay Host release (download, verification, hand-over to the service).
 *
 * The host itself decides what is installed: the release found by update_check. A request from the Web UI or the
 * tray can only start, cancel or hurry the installation of that release; it never carries a URL or a path.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// local includes
#include "update_check.h"

namespace update_install {
  /// Upper bound for the installer download.
  constexpr std::uint64_t max_installer_bytes = 200ull * 1024 * 1024;
  /// Upper bound for the signed checksum list and the signature.
  constexpr std::uint64_t max_sums_bytes = 64 * 1024;
  constexpr std::uint64_t max_signature_bytes = 1024;
  /// Redirects followed per download.
  constexpr int max_redirects = 5;
  /// Files in the updates folder that are older than this are removed at start.
  constexpr std::chrono::hours stale_after {24};

  enum class state_e {
    idle,  ///< No newer release is known.
    available,  ///< A newer release is known, nothing was downloaded.
    downloading,
    verifying,
    waiting_for_stream,  ///< Verified and ready; waits for the running stream to end.
    installing,  ///< The installer was handed to the service. The host is about to be replaced.
    failed,
  };

  enum class error_e {
    none,
    no_connection,
    verify_failed,  ///< Signature or hash check failed; the download was deleted.
    no_space,
    cancelled,
    not_supported,  ///< Not running under the service or without the rights to install.
    launch_failed,
    installer_failed,
    internal,
  };

  /// Stable names for the JSON API.
  const char *to_string(state_e state);
  const char *to_string(error_e error);

  struct status_t {
    state_e state = state_e::idle;
    error_e error = error_e::none;
    std::string version;  ///< Version text of the candidate, e.g. `2026.11.20.1`; empty when none.
    std::string release_url;  ///< Release page of the candidate (always a project link).
    int percent = 0;  ///< Download progress, 0-100.
    bool stream_active = false;
    bool can_install = false;
  };

  /// Result of a download.
  enum class transfer_e {
    ok,
    network,
    too_large,
    no_space,
    cancelled,
    rejected,  ///< The URL (or a redirect target) is not an allowed release location.
  };

  /// Result of handing the installer to the service.
  enum class launch_e {
    ok,
    not_supported,
    failed,
  };

  // ---- pure helpers ------------------------------------------------------------------------------------------

  /// Version text of a release tag (`host-v2026.11.20.1` -> `2026.11.20.1`), or nothing if the tag is not safe.
  std::optional<std::string> version_text(std::string_view tag);

  /// `NyxPlayHost-<version>-windows-x64.exe`
  std::string installer_name(std::string_view version);

  /// Download locations of a release asset, preferred first (GitHub, then the GitLab mirror).
  std::vector<std::string> asset_urls(std::string_view tag, std::string_view asset);

  /// Whether a download URL, or a redirect target, may be fetched.
  bool is_allowed_url(std::string_view url);

  /**
   * @brief Parse the body of an install or cancel request.
   * @return The `now` flag, or nothing when the body holds anything but an optional boolean `now`.
   */
  std::optional<bool> parse_request(std::string_view body);

  /// SDDL of the updates folder: SYSTEM and Administrators only, no inheritance from the parent.
  const char *updates_dir_sddl();

  /**
   * @brief Decide whether the host has just been updated.
   * @param marker The stored marker (`<previous> <target>`), empty if none.
   * @param current The running version text.
   * @return The version to announce, or nothing.
   */
  std::optional<std::string> updated_notice(std::string_view marker, std::string_view current);

  // ---- controller --------------------------------------------------------------------------------------------

  using progress_fn = std::function<void(std::uint64_t received, std::uint64_t total)>;

  /// Everything the controller needs from the outside, replaceable in tests.
  struct environment_t {
    std::filesystem::path updates_dir;
    std::string current_version;
    /// Creates the folder with the restricted ACL. Returns false when that is not possible.
    std::function<bool(const std::filesystem::path &)> prepare_dir;
    std::function<transfer_e(std::string_view url, const std::filesystem::path &dest, std::uint64_t max_bytes, const std::atomic<bool> &cancel, const progress_fn &progress)> download;
    std::function<bool()> stream_active;
    std::function<bool()> can_install;
    /// Hands the verified installer to the service. Takes the file name inside the updates folder and its SHA-256.
    std::function<launch_e(const std::filesystem::path &installer, const std::string &sha256)> launch;
    /// Exit code reported by the service for the installer, once it has ended.
    std::function<std::optional<int>()> install_result;
    std::function<void(std::string_view marker)> store_marker;
    /// Called on every visible change; `notify` marks the moments that deserve a notification.
    std::function<void(const status_t &status, bool notify)> publish;
    std::chrono::milliseconds poll_interval {1000};
    std::chrono::seconds install_timeout {15 * 60};
  };

  enum class request_e {
    started,
    already_running,
    no_update,
    busy_installing,
  };

  class controller_t {
  public:
    explicit controller_t(environment_t env);
    ~controller_t();

    controller_t(const controller_t &) = delete;
    controller_t &operator=(const controller_t &) = delete;

    /// Announce the release the host found. Ignored while an installation is running.
    void set_candidate(const update_check::release_t &release);

    /// Start the installation of the candidate; `now` also installs while a stream is running.
    request_e request_install(bool now);

    /// Cancel a download, verification or wait. Returns false when nothing can be cancelled.
    bool cancel();

    status_t status() const;

    /// Cancel and wait for the worker. The destructor does the same.
    void shutdown();

  private:
    void run();
    void set_state(state_e state, error_e error, bool notify = false);
    void fail(error_e error);
    void cleanup_files();
    bool cancelled() const;

    environment_t env;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> cancel_flag {false};
    bool stopping = false;
    bool install_now = false;
    status_t current;
    std::string candidate_tag;
    int notified_bucket = -1;
  };

  // ---- host integration --------------------------------------------------------------------------------------

  /// Create the production controller, announce a finished update and remove stale files. Call once at start.
  void init();

  /// Stop the production controller. Safe to call when it was never started.
  void shutdown();

  void set_candidate(const update_check::release_t &release);
  request_e request_install(bool now);
  bool cancel();
  status_t status();
}  // namespace update_install
