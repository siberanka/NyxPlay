/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <csignal>
#include <filesystem>
#include <format>
#include <iostream>
#include <thread>

// local includes
#include "config.h"
#include "config_import.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"

extern "C" {
#ifdef _WIN32
  #include <iphlpapi.h>
#endif
}

using namespace std::literals;

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

namespace args {
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name);
    }

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

  int import_config(const char *name, int argc, char *argv[]) {
    const auto target_dir = std::filesystem::path(config::sunshine.config_file).parent_path();

    std::string source_arg = "auto";
    config_import::options_t options;
    for (int i = 0; i < argc; ++i) {
      std::string_view arg {argv[i]};
      if (arg == "--no-settings"sv) {
        options.settings = false;
      } else if (arg == "--no-apps"sv) {
        options.apps = false;
      } else if (arg == "--no-clients"sv) {
        options.clients = false;
      } else if (arg == "--web-credentials"sv) {
        options.web_credentials = true;
      } else if (arg == "--default-port"sv) {
        options.keep_port = false;
      } else if (arg == "help"sv || arg == "--help"sv) {
        std::cout
          << "Usage: "sv << name << " --import-config [auto|detect|<config directory>] [options]"sv << std::endl
          << "  auto                 import from the first detected Apollo or Sunshine installation (default)"sv << std::endl
          << "  detect               only list the detected installations (exit code 0 if one exists)"sv << std::endl
          << "  <config directory>   import from this directory (contains sunshine.conf)"sv << std::endl
          << "Options:"sv << std::endl
          << "  --no-settings        do not import sunshine.conf"sv << std::endl
          << "  --no-apps            do not import the application list and covers"sv << std::endl
          << "  --no-clients         do not import paired clients and the host identity"sv << std::endl
          << "  --web-credentials    also import the Web UI user name and password"sv << std::endl
          << "  --default-port       do not import the port setting"sv << std::endl
          << "Stop the host before importing; existing files are backed up."sv << std::endl;
        return 0;
      } else if (!arg.starts_with("--"sv)) {
        source_arg = arg;
      } else {
        std::cout << "Unknown option: "sv << arg << std::endl;
        return 2;
      }
    }

    std::filesystem::path source_dir;
    if (source_arg == "auto"sv || source_arg == "detect"sv) {
      const auto sources = config_import::detect_sources(target_dir);
      if (sources.empty()) {
        std::cout << "No Apollo or Sunshine configuration was found."sv << std::endl;
        return 1;
      }
      for (const auto &source : sources) {
        std::cout << "Found "sv << source.product << " configuration: "sv << source.config_dir.string() << std::endl;
      }
      if (source_arg == "detect"sv) {
        return 0;
      }
      source_dir = sources.front().config_dir;
    } else {
      source_dir = std::filesystem::path(source_arg);
    }

    const auto result = config_import::import_from(source_dir, target_dir, options);
    for (const auto &note : result.warnings) {
      BOOST_LOG(warning) << "Import: "sv << note;
    }
    if (!result.ok) {
      BOOST_LOG(error) << "Import from ["sv << source_dir.string() << "] failed: "sv << result.error;
      return 3;
    }

    for (const auto &item : result.imported) {
      BOOST_LOG(info) << "Imported "sv << item;
    }
    if (!result.backup_dir.empty()) {
      BOOST_LOG(info) << "Previous files were saved to ["sv << result.backup_dir.string() << ']';
    }
    BOOST_LOG(info) << "Import from ["sv << source_dir.string() << "] finished"sv;
    return 0;
  }

#ifdef _WIN32
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif
}  // namespace args

namespace lifetime {
  char **argv;
  std::atomic_int desired_exit_code;

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
    // If debug trap still doesn't work, abort
    abort();
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  class service_controller {
  public:
    /**
     * @brief Constructor for service_controller class.
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      service_handle = OpenServiceA(scm_handle, "NyxPlayHostService", service_desired_access);
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << SERVICE_NAME " failed to start: "sv << status.dwWin32ExitCode;
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
