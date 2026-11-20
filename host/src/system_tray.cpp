/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
    #include "platform/windows/utils.h"
    #define TRAY_ICON WEB_DIR "images/nyxplay-host.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/nyxplay-host-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/nyxplay-host-pausing.ico"
    #define TRAY_ICON_LOCKED WEB_DIR "images/nyxplay-host-locked.ico"
  #elif defined(__linux__) || defined(linux) || defined(__linux)
    #define TRAY_ICON SUNSHINE_TRAY_PREFIX "-tray"
    #define TRAY_ICON_PLAYING SUNSHINE_TRAY_PREFIX "-playing"
    #define TRAY_ICON_PAUSING SUNSHINE_TRAY_PREFIX "-pausing"
    #define TRAY_ICON_LOCKED SUNSHINE_TRAY_PREFIX "-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-nyxplay-host-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/nyxplay-host-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/nyxplay-host-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/nyxplay-host-locked-16.png"
    #include <dispatch/dispatch.h>
  #endif

  #define TRAY_MSG_NO_APP_RUNNING "Reload Apps"

  #ifndef BOOST_PROCESS_VERSION
    #define BOOST_PROCESS_VERSION 1
  #endif

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <cctype>
  #include <chrono>
  #include <csignal>
  #include <format>
  #include <functional>
  #include <mutex>
  #include <string>
  #include <thread>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "config.h"
  #include "confighttp.h"
  #include "display_device.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "system_tray.h"
  #include "update_install.h"
  #include "network.h"
  #include "src/entry_handler.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic tray_initialized = false;

  // Threading variables for all platforms
  static std::thread tray_thread;
  static std::atomic tray_thread_running = false;
  static std::atomic tray_thread_should_exit = false;

  // Latest release notice that arrived while the tray did not exist yet; guarded by the mutex,
  // which also orders it against tray_initialized becoming true
  static std::mutex pending_update_mutex;
  static std::string pending_update_version;

  bool wait_for_shell(const std::function<bool()> &shell_ready, const std::atomic<bool> &stop, std::chrono::milliseconds poll) {
    while (!stop) {
      if (shell_ready()) {
        return true;
      }
      // Sleep in short slices so a stop request is honoured promptly
      for (auto left = poll; left.count() > 0 && !stop; left -= std::min(left, 100ms)) {
        std::this_thread::sleep_for(std::min(left, 100ms));
      }
    }
    return false;
  }

  std::string take_pending_update_notice() {
    std::lock_guard lg {pending_update_mutex};
    std::string out;
    out.swap(pending_update_version);
    return out;
  }

  #ifdef _WIN32
  static bool shell_ready() {
    return GetShellWindow() != nullptr && FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
  }
  #endif

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void
  tray_force_stop_cb(struct tray_menu *item) {
    BOOST_LOG(info) << "Force stop from system tray"sv;
    proc::proc.terminate();
  }

  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;

    std::ignore = display_device::reset_persistence();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    proc::proc.terminate();
    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

    proc::proc.terminate();

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu =
      (struct tray_menu[16]) {  // spare room: update entries are inserted before "Restart"
        // todo - use boost/locale to translate menu strings
        { .text = "Open NyxPlay Host", .cb = tray_open_ui_cb },
        { .text = "-" },
        // { .text = "-" },
        // { .text = "Donate",
        //   .submenu =
        //     (struct tray_menu[]) {
        //       { .text = "GitHub Sponsors", .cb = tray_donate_github_cb },
        //       { .text = "MEE6", .cb = tray_donate_mee6_cb },
        //       { .text = "Patreon", .cb = tray_donate_patreon_cb },
        //       { .text = "PayPal", .cb = tray_donate_paypal_cb },
        //       { .text = nullptr } } },
        // { .text = "-" },
        { .text = TRAY_MSG_NO_APP_RUNNING, .cb = tray_force_stop_cb },
  // Currently display device settings are only supported on Windows
  #ifdef _WIN32
        {.text = "Reset Display Device Config", .cb = tray_reset_display_device_config_cb},
  #endif
        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  // ---- One-click update: menu entries, tooltip and notifications ---------------------------------------------

  namespace {
    std::mutex ui_mutex;
    update_ui_t latest_ui;  // drives the menu
    update_ui_t notice_ui;  // the newest state that asked for a notification
    bool latest_dirty = false;
    bool notice_dirty = false;
    std::string menu_release_url;
    std::size_t restart_index = 0;
    struct tray_menu restart_item {};
    struct tray_menu quit_item {};
    bool menu_captured = false;

    bool turkish_ui() {
      return config::sunshine.locale == "tr";
    }

    std::string plain_version(std::string version) {
      if (version.starts_with("host-")) {
        version.erase(0, 5);
      }
      version.erase(std::remove_if(version.begin(), version.end(), [](unsigned char c) {
        return !(std::isalnum(c) || c == '.' || c == '-');
      }), version.end());
      if (version.size() > 40) {
        version.resize(40);
      }
      return version;
    }

    /// Short, actionable text for an error code of the installation (UTF-8).
    std::string reason_text(const std::string &code, bool tr) {
      if (code == "no_connection") {
        return tr ? "Ba\xC4\x9Flant\xC4\xB1 yok" : "No connection";
      }
      if (code == "verify_failed") {
        return tr ? "\xC4\xB0ndirme do\xC4\x9Frulanamad\xC4\xB1 ve silindi" : "The download could not be verified and was deleted";
      }
      if (code == "no_space") {
        return tr ? "Yeterli disk alan\xC4\xB1 yok" : "Not enough disk space";
      }
      if (code == "not_supported") {
        return tr ? "G\xC3\xBCncelleme buradan y\xC3\xBCklenemiyor" : "The update cannot be installed from here";
      }
      if (code == "launch_failed") {
        return tr ? "Y\xC3\xBCkleyici ba\xC5\x9Flat\xC4\xB1lamad\xC4\xB1" : "The installer could not be started";
      }
      if (code == "installer_failed") {
        return tr ? "Y\xC3\xBCkleyici ba\xC5\x9F" "ar\xC4\xB1s\xC4\xB1z oldu" : "The installer failed";
      }
      return tr ? "Beklenmeyen hata" : "Unexpected error";
    }

    // The tray keeps the pointers of menu texts and notifications: they live in static buffers
    char menu_text[3][224];
    char note_title[128];
    char note_text[320];
    char note_tooltip[96];

    std::string to_native(const std::string &text) {
  #ifdef _WIN32
      return utf8ToAcp(text);
  #else
      return text;
  #endif
    }

    void copy_to(char *target, std::size_t size, const std::string &text) {
      snprintf(target, size, "%s", to_native(text).c_str());
    }

    void update_install_cb([[maybe_unused]] struct tray_menu *item) {
      BOOST_LOG(info) << "Installing the update from the system tray"sv;
      update_install::request_install(false);
    }

    void update_install_now_cb([[maybe_unused]] struct tray_menu *item) {
      BOOST_LOG(info) << "Installing the update now (ends the stream) from the system tray"sv;
      update_install::request_install(true);
    }

    void update_cancel_cb([[maybe_unused]] struct tray_menu *item) {
      BOOST_LOG(info) << "Cancelling the update from the system tray"sv;
      update_install::cancel();
    }

    void update_page_cb([[maybe_unused]] struct tray_menu *item) {
      std::string url;
      {
        std::lock_guard lg {ui_mutex};
        url = menu_release_url;
      }
      if (!url.empty()) {
        platf::open_url(url);
      }
    }

    void capture_menu() {
      if (menu_captured) {
        return;
      }
      menu_captured = true;
      for (std::size_t i = 0; tray.menu[i].text != nullptr; ++i) {
        if (tray.menu[i].cb == tray_restart_cb) {
          restart_index = i;
          restart_item = tray.menu[i];
          quit_item = tray.menu[i + 1];
          return;
        }
      }
    }

    /// Rebuild the entries between the standard ones and "Restart" for the given state. Runs on the tray thread.
    void apply_menu(const update_ui_t &ui) {
      capture_menu();
      if (restart_index == 0) {
        return;
      }
      const bool tr = turkish_ui();
      const auto version = plain_version(ui.version);
      const std::string install_text = tr ? "G\xC3\xBCncellemeyi y\xC3\xBCkle: " + version + "\xE2\x80\xA6" : "Install update " + version + "\xE2\x80\xA6";
      std::size_t count = 0;
      struct tray_menu extra[3] {};

      switch (ui.state) {
        case update_ui_e::available:
          copy_to(menu_text[0], sizeof(menu_text[0]), install_text);
          extra[count++] = {.text = menu_text[0], .cb = update_install_cb};
          break;
        case update_ui_e::downloading:
        case update_ui_e::verifying:
          copy_to(menu_text[0], sizeof(menu_text[0]), tr ? "G\xC3\xBCncelleniyor\xE2\x80\xA6 %" + std::to_string(ui.percent) + " - \xC4\xB0ptal et" : "Updating\xE2\x80\xA6 " + std::to_string(ui.percent) + " % - Cancel");
          extra[count++] = {.text = menu_text[0], .cb = update_cancel_cb};
          break;
        case update_ui_e::waiting:
          copy_to(menu_text[0], sizeof(menu_text[0]), tr ? "\xC5\x9Eimdi y\xC3\xBCkle (yay\xC4\xB1n\xC4\xB1 sonland\xC4\xB1r\xC4\xB1r)" : "Install now (ends the stream)");
          extra[count++] = {.text = menu_text[0], .cb = update_install_now_cb};
          copy_to(menu_text[1], sizeof(menu_text[1]), tr ? "G\xC3\xBCncellemeyi iptal et" : "Cancel update");
          extra[count++] = {.text = menu_text[1], .cb = update_cancel_cb};
          break;
        case update_ui_e::installing:
          copy_to(menu_text[0], sizeof(menu_text[0]), tr ? "G\xC3\xBCncelleme y\xC3\xBCkleniyor\xE2\x80\xA6" : "Installing update\xE2\x80\xA6");
          extra[count++] = {.text = menu_text[0], .disabled = 1};
          break;
        case update_ui_e::failed:
          copy_to(menu_text[0], sizeof(menu_text[0]), (tr ? "G\xC3\xBCncelleme ba\xC5\x9F" "ar\xC4\xB1s\xC4\xB1z: " : "Update failed: ") + reason_text(ui.reason, tr) + (tr ? " - s\xC3\xBCr\xC3\xBCm sayfas\xC4\xB1n\xC4\xB1 a\xC3\xA7" : " - open release page"));
          extra[count++] = {.text = menu_text[0], .cb = update_page_cb};
          copy_to(menu_text[1], sizeof(menu_text[1]), install_text);
          extra[count++] = {.text = menu_text[1], .cb = update_install_cb};
          break;
        default:
          break;
      }

      std::size_t pos = restart_index;
      for (std::size_t i = 0; i < count; ++i) {
        tray.menu[pos++] = extra[i];
      }
      tray.menu[pos++] = restart_item;
      tray.menu[pos++] = quit_item;
      tray.menu[pos] = {};
    }

    void open_release_page_cb() {
      update_page_cb(nullptr);
    }

    void open_ui_cb() {
      launch_ui();
    }

    void show_notice(const update_ui_t &ui) {
      const bool tr = turkish_ui();
      const auto version = plain_version(ui.version);
      std::string title = tr ? "NyxPlay Host g\xC3\xBCncellemesi" : "NyxPlay Host update";
      std::string text;
      bool open_page = false;
      switch (ui.state) {
        case update_ui_e::downloading:
          text = (tr ? "G\xC3\xBCncelleme indiriliyor: " : "Downloading the update: ") + version + " (" + std::to_string(ui.percent) + " %)";
          break;
        case update_ui_e::verifying:
          text = tr ? "G\xC3\xBCncelleme do\xC4\x9Frulan\xC4\xB1yor\xE2\x80\xA6" : "Verifying the update\xE2\x80\xA6";
          break;
        case update_ui_e::waiting:
          text = tr ? "G\xC3\xBCncelleme yay\xC4\xB1n bitti\xC4\x9Finde y\xC3\xBCklenecek." : "The update will be installed when the stream ends.";
          break;
        case update_ui_e::installing:
          text = (tr ? "NyxPlay Host y\xC3\xBCkleniyor: " : "Installing NyxPlay Host ") + version + "\xE2\x80\xA6";
          break;
        case update_ui_e::failed:
          title = tr ? "G\xC3\xBCncelleme ba\xC5\x9F" "ar\xC4\xB1s\xC4\xB1z" : "Update failed";
          text = reason_text(ui.reason, tr) + (tr ? " - s\xC3\xBCr\xC3\xBCm sayfas\xC4\xB1n\xC4\xB1 a\xC3\xA7" : " - open release page");
          open_page = true;
          break;
        case update_ui_e::updated:
          title = tr ? "NyxPlay Host g\xC3\xBCncellendi" : "NyxPlay Host updated";
          text = tr ? "NyxPlay Host " + version + " s\xC3\xBCr\xC3\xBCm\xC3\xBCne g\xC3\xBCncellendi" : "NyxPlay Host updated to " + version;
          break;
        default:
          return;
      }
      copy_to(note_title, sizeof(note_title), title);
      copy_to(note_text, sizeof(note_text), text);

      tray.notification_title = nullptr;
      tray.notification_text = nullptr;
      tray.notification_cb = nullptr;
      tray.notification_icon = nullptr;
      tray_update(&tray);
      tray.notification_icon = TRAY_ICON;
      tray.notification_title = note_title;
      tray.notification_text = note_text;
      tray.notification_cb = open_page ? open_release_page_cb : open_ui_cb;
    }

    /// Runs on the tray thread: applies what update_tray_update_state() queued.
    void apply_update_ui() {
      update_ui_t menu_ui;
      update_ui_t notice;
      bool do_menu = false;
      bool do_notice = false;
      {
        std::lock_guard lg {ui_mutex};
        if (!latest_dirty && !notice_dirty) {
          return;
        }
        do_menu = latest_dirty;
        do_notice = notice_dirty;
        menu_ui = latest_ui;
        notice = notice_ui;
        latest_dirty = notice_dirty = false;
        if (!menu_ui.url.empty()) {
          menu_release_url = menu_ui.url;
        }
      }

      if (do_menu) {
        apply_menu(menu_ui);
        const bool busy = menu_ui.state == update_ui_e::downloading || menu_ui.state == update_ui_e::verifying;
        if (busy) {
          const bool tr = turkish_ui();
          const auto text = (tr ? "G\xC3\xBCncelleme indiriliyor (%" : "Downloading update (") + std::to_string(menu_ui.percent) + (tr ? ")" : " %)");
          copy_to(note_tooltip, sizeof(note_tooltip), std::string {PROJECT_NAME} + " - " + text);
          tray.tooltip = note_tooltip;
        } else {
          tray.tooltip = PROJECT_NAME;
        }
      }
      if (do_notice) {
        show_notice(notice);
      }
      tray_update(&tray);
    }
  }  // namespace

  void update_tray_update_state(update_ui_t ui) {
    std::lock_guard lg {ui_mutex};
    if (ui.notify) {
      notice_ui = ui;
      notice_dirty = true;
    }
    if (ui.state != update_ui_e::updated) {
      latest_ui = std::move(ui);
      latest_dirty = true;
    }
  }


  int init_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell (desktop and taskbar) before registering the tray icon, which may not
    // exist yet when the host starts before anyone logs on. Gives up when asked to stop.
    const auto wait_start = std::chrono::steady_clock::now();
    if (!shell_ready()) {
      BOOST_LOG(info) << "Waiting for the desktop shell before creating the system tray"sv;
      if (!wait_for_shell(shell_ready, tray_thread_should_exit)) {
        BOOST_LOG(info) << "System tray start cancelled before the desktop shell appeared"sv;
        return 2;
      }
    }

    // The host may run with higher integrity than Explorer (SYSTEM service), and UIPI would drop
    // the broadcast that tells us to re-add the icon after Explorer restarts.
    ChangeWindowMessageFilter(RegisterWindowMessageA("TaskbarCreated"), MSGFLT_ADD);
  #endif

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }

    std::string pending;
    {
      // Publish readiness and take a notice that arrived earlier under one lock, so none is lost
      std::lock_guard lg {pending_update_mutex};
      tray_initialized = true;
      pending.swap(pending_update_version);
    }
  #ifdef _WIN32
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - wait_start).count();
    BOOST_LOG(info) << "System tray created after "sv << waited << " s"sv;
  #else
    BOOST_LOG(info) << "System tray created"sv;
  #endif

    if (!pending.empty()) {
      update_tray_update_available(std::move(pending));
    }
    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      return 1;
    }

    apply_update_ui();

    // Process one iteration of the tray loop with non-blocking mode (0)
    if (const int result = tray_loop(0); result != 0) {
      BOOST_LOG(warning) << "System tray loop failed"sv;
      return result;
    }

    return 0;
  }

  int end_tray() {
    if (tray_initialized) {
      tray_initialized = false;
      tray_exit();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PLAYING;

    tray_update(&tray);
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "App launched";
    char msg[256];
    static char force_close_msg[256];
    snprintf(msg, std::size(msg), "%s launched.", app_name.c_str());
    snprintf(force_close_msg, std::size(force_close_msg), "Force close [%s]", app_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
    strncpy(force_close_msg, utf8ToAcp(force_close_msg).c_str(), std::size(force_close_msg) - 1);
  #endif
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray.tooltip = PROJECT_NAME;
    tray.menu[2].text = force_close_msg;
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PAUSING;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Streaming paused for %s", app_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = "Stream Paused";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Streaming stopped for %s", app_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Application Stopped";
    tray.notification_text = msg;
    tray.tooltip = PROJECT_NAME;
    tray.menu[2].text = TRAY_MSG_NO_APP_RUNNING;
    tray_update(&tray);
  }

  void
  update_tray_launch_error(std::string app_name, int exit_code) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Application %s exited too fast with code %d. Click here to terminate the stream.", app_name.c_str(), exit_code);
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Launch Error";
    tray.notification_text = msg;
    tray.notification_cb = []() {
      BOOST_LOG(info) << "Force stop from notification"sv;
      proc::proc.terminate();
    };
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_require_pin() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    tray.icon = TRAY_ICON;
    tray.notification_title = "Incoming Pairing Request";
    tray.notification_text = "Click here to complete the pairing process";
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/pin#PIN");
    };
    tray_update(&tray);
  }

  void
  update_tray_paired(std::string device_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "Device %s paired Succesfully. Please make sure you have access to the device.", device_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.notification_title = "Device Paired Succesfully";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void
  update_tray_client_connected(std::string client_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    char msg[256];
    snprintf(msg, std::size(msg), "%s has connected to the session.", client_name.c_str());
  #ifdef _WIN32
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif
    tray.notification_title = "Client Connected";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_update_available(std::string version) {
    {
      std::lock_guard lg {pending_update_mutex};
      if (!tray_initialized) {
        // Shown once, right after the tray has been created; a newer notice replaces it
        pending_update_version = std::move(version);
        return;
      }
    }

    // The notification texts must outlive the call: the tray keeps the pointers
    static char title[128];
    static char msg[256];

    const bool turkish = config::sunshine.locale == "tr";
    // Keep the shown text to a plain version number
    if (version.starts_with("host-")) {
      version.erase(0, 5);
    }
    version.erase(std::remove_if(version.begin(), version.end(), [](unsigned char c) {
      return !(std::isalnum(c) || c == '.' || c == '-');
    }), version.end());
    if (version.size() > 40) {
      version.resize(40);
    }

    snprintf(title, std::size(title), turkish ? "NyxPlay Host %s s\xC3\xBCr\xC3\xBCm\xC3\xBC mevcut" : "NyxPlay Host %s is available", version.c_str());
    snprintf(msg, std::size(msg), "%s", turkish ? "Yenilikleri g\xC3\xB6rmek i\xC3\xA7in Web aray\xC3\xBCz\xC3\xBCn\xC3\xBC a\xC3\xA7\xC4\xB1n." : "Open the Web UI to see what's new.");
  #ifdef _WIN32
    strncpy(title, utf8ToAcp(title).c_str(), std::size(title) - 1);
    strncpy(msg, utf8ToAcp(msg).c_str(), std::size(msg) - 1);
  #endif

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray_update(&tray);
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = title;
    tray.notification_text = msg;
    tray.notification_cb = []() {
      launch_ui();
    };
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread; this waits for the shell if it is not up yet
    if (const int result = init_tray(); result != 0) {
      if (result != 2) {
        BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      }
      tray_thread_running = false;
      return;
    }

    tray_thread_running = true;

    // Main tray event loop
    while (!tray_thread_should_exit) {
      if (process_tray_events() != 0) {
        BOOST_LOG(warning) << "Tray event processing failed in thread"sv;
        break;
      }

      // Sleep to avoid busy waiting
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Clean up the tray
    end_tray();
    tray_thread_running = false;
    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    if (tray_thread.joinable()) {
      BOOST_LOG(warning) << "Tray thread is already running"sv;
      return 1;
    }

  #ifdef _WIN32
    std::string tmp_str = "Open NyxPlay Host (" + config::nvhttp.sunshine_name + ":" + std::to_string(net::map_port(confighttp::PORT_HTTPS)) + ")";
    static const std::string title_str = utf8ToAcp(tmp_str);
  #else
    static const std::string title_str = "Open NyxPlay Host (" + config::nvhttp.sunshine_name + ":" + std::to_string(net::map_port(confighttp::PORT_HTTPS)) + ")";
  #endif
    tray.menu[0].text = title_str.c_str();

    if (config::sunshine.hide_tray_controls) {
      tray.menu[1].text = nullptr;
    }

    tray_thread_should_exit = false;

    // Do not wait for the tray: when the host starts before a user logs on, the shell (and so
    // the tray) only appears later, and startup must not be held up until then.
    try {
      tray_thread = std::thread(tray_thread_worker);
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

  int end_tray_threaded() {
    // The thread may still be waiting for the shell, so join whenever it was started
    if (!tray_thread.joinable()) {
      return 0;
    }

    BOOST_LOG(info) << "Stopping system tray thread"sv;
    tray_thread_should_exit = true;
    tray_thread.join();

    BOOST_LOG(info) << "System tray thread stopped"sv;
    return 0;
  }

}  // namespace system_tray

  #ifdef BOOST_PROCESS_VERSION
    #undef BOOST_PROCESS_VERSION 1
  #endif

#endif
