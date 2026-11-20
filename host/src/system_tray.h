/**
 * @file src/system_tray.h
 * @brief Declarations for the system tray icon and notification system.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

/**
 * @brief Handles the system tray icon and notification system.
 */
namespace system_tray {
  /**
   * @brief Callback for opening the UI from the system tray.
   * @param item The tray menu item.
   */
  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item);


  void tray_force_stop_cb(struct tray_menu *item);

  /**
   * @brief Callback for resetting display device configuration.
   * @param item The tray menu item.
   */
  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for restarting Sunshine from the system tray.
   * @param item The tray menu item.
   */
  void tray_restart_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Callback for exiting Sunshine from the system tray.
   * @param item The tray menu item.
   */
  void tray_quit_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief Initializes the system tray without starting a loop.
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray();

  /**
   * @brief Processes a single tray event iteration.
   * @return 0 if processing was successful, non-zero otherwise.
   */
  int process_tray_events();

  /**
   * @brief Exit the system tray.
   * @return 0 after exiting the system tray.
   */
  int end_tray();

  /**
   * @brief Sets the tray icon in playing mode and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_playing(std::string app_name);

  /**
   * @brief Sets the tray icon in pausing mode (stream stopped but app running) and spawns the appropriate notification
   * @param app_name The paused application name
   */
  void update_tray_pausing(std::string app_name);

  /**
   * @brief Sets the tray icon in stopped mode (app and stream stopped) and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_stopped(std::string app_name);

  void
  update_tray_launch_error(std::string app_name, int exit_code);

  /**
   * @brief Spawns a notification for PIN Pairing. Clicking it opens the PIN Web UI Page
   */
  void update_tray_require_pin();

  void update_tray_paired(std::string device_name);

  void update_tray_client_connected(std::string client_name);

  /**
   * @brief Spawns a notification about a new release. Clicking it opens the Web UI dashboard
   * @param version The release tag
   */
  void update_tray_update_available(std::string version);

  /**
   * @brief The state of the one-click update, as far as the tray shows it.
   */
  enum class update_ui_e {
    none,
    available,
    downloading,
    verifying,
    waiting,  ///< Verified, waits for the running stream to end.
    installing,
    failed,
    updated,  ///< The host was just updated; shown once.
  };

  struct update_ui_t {
    update_ui_e state = update_ui_e::none;
    std::string version;  ///< Plain version text, e.g. `2026.11.20.1`.
    int percent = 0;
    std::string reason;  ///< Error code of the failed state.
    std::string url;  ///< Release page.
    bool notify = false;  ///< Also show a notification.
  };

  /**
   * @brief Show the update state: menu entries, tooltip and, when requested, one notification.
   *        The latest state wins; it is applied by the tray thread (also when the tray is created later).
   */
  void update_tray_update_state(update_ui_t ui);

  /**
   * @brief Initializes and runs the system tray in a separate thread.
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray_threaded();

  /**
   * @brief Stops the threaded system tray and waits for the thread to finish.
   * @return 0 after stopping the threaded tray.
   */
  int end_tray_threaded();

  /**
   * @brief Waits until the desktop shell is available.
   * @param shell_ready Returns true once the shell can host a tray icon.
   * @param stop Set to true to give up waiting.
   * @param poll How often @p shell_ready is evaluated.
   * @return true if the shell became available, false if @p stop was set first.
   */
  bool wait_for_shell(const std::function<bool()> &shell_ready, const std::atomic<bool> &stop, std::chrono::milliseconds poll = std::chrono::milliseconds(1000));

  /**
   * @brief Takes the release notice that was queued while the tray did not exist yet.
   * @return The queued release tag, or an empty string if there is none.
   */
  std::string take_pending_update_notice();
}  // namespace system_tray
