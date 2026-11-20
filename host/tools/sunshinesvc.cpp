/**
 * @file tools/sunshinesvc.cpp
 * @brief Handles launching Sunshine.exe into user sessions as SYSTEM
 */
#define WIN32_LEAN_AND_MEAN
#include "svc_logic.h"

#include <format>
#include <functional>
#include <string>
#include <vector>
#include <winsock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <sddl.h>
#include <WtsApi32.h>

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

SERVICE_STATUS_HANDLE service_status_handle;
SERVICE_STATUS service_status;
SRWLOCK service_status_lock = SRWLOCK_INIT;
HANDLE stop_event;
HANDLE session_change_event;
HANDLE event_source;
// Auto-reset event (SYSTEM only) with which the host asks for a verified update installer to be run, and the
// installer process while it runs. The installer must outlive this service, so it is not part of the host's job.
HANDLE update_event = nullptr;
HANDLE update_process = nullptr;

#define SERVICE_NAME "NyxPlayHostService"

// Time budget of a stop request. SCM is told to wait 30 s, the steps below add up to less.
constexpr DWORD STOP_WAIT_HINT_MS = 30 * 1000;
constexpr DWORD TERMINATION_HELPER_TIMEOUT_MS = 8 * 1000;
constexpr DWORD HOST_EXIT_TIMEOUT_MS = 15 * 1000;
// How often a host that is not listening yet is checked by the startup watchdog.
constexpr DWORD HOST_READY_POLL_MS = 2 * 1000;

/**
 * @brief Publish the service status. Called from the control handler thread and from ServiceMain().
 */
static void ReportStatus(DWORD state, DWORD controls_accepted, DWORD wait_hint, DWORD exit_code = NO_ERROR) {
  AcquireSRWLockExclusive(&service_status_lock);
  service_status.dwCurrentState = state;
  service_status.dwControlsAccepted = controls_accepted;
  service_status.dwWaitHint = wait_hint;
  service_status.dwWin32ExitCode = exit_code;
  service_status.dwCheckPoint = (state == SERVICE_STOP_PENDING || state == SERVICE_START_PENDING) ? service_status.dwCheckPoint + 1 : 0;
  SetServiceStatus(service_status_handle, &service_status);
  ReleaseSRWLockExclusive(&service_status_lock);
}

/**
 * @brief Report progress while a stop request is being processed so SCM does not give up on us.
 */
static void ReportStopProgress() {
  AcquireSRWLockExclusive(&service_status_lock);
  if (service_status.dwCurrentState == SERVICE_STOP_PENDING) {
    service_status.dwCheckPoint++;
    service_status.dwWaitHint = STOP_WAIT_HINT_MS;
    SetServiceStatus(service_status_handle, &service_status);
  }
  ReleaseSRWLockExclusive(&service_status_lock);
}

/**
 * @brief Wait for @p handle for at most @p timeout_ms while reporting stop progress every second.
 * @return The result of the last WaitForSingleObject() call.
 */
static DWORD WaitReportingProgress(HANDLE handle, DWORD timeout_ms) {
  DWORD waited = 0;
  for (;;) {
    DWORD slice = std::min<DWORD>(1000u, timeout_ms - waited);
    DWORD result = WaitForSingleObject(handle, slice);
    if (result != WAIT_TIMEOUT) {
      return result;
    }
    waited += slice;
    ReportStopProgress();
    if (waited >= timeout_ms) {
      return WAIT_TIMEOUT;
    }
  }
}

/**
 * @brief Append a line to the service log (shared with the host's standard output).
 */
static void LogLine(HANDLE log_file, const std::string &message) {
  if (log_file == nullptr || log_file == INVALID_HANDLE_VALUE) {
    return;
  }

  SYSTEMTIME time;
  GetLocalTime(&time);
  auto line = std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}] NyxPlayHostService: {}\r\n", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, message);

  DWORD written;
  WriteFile(log_file, line.data(), (DWORD) line.size(), &written, nullptr);
}

/**
 * @brief Write a failure to the log file and, once per distinct failure, to the Application event log.
 */
static void ReportFailure(HANDLE log_file, const std::string &step, DWORD error, DWORD &last_reported_key) {
  auto message = std::format("{} failed with error {}", step, error);
  LogLine(log_file, message);

  // Remember what was reported last so a crash loop does not flood the event log
  DWORD key = (DWORD) (std::hash<std::string> {}(step.substr(0, step.find(" (retry"))) ^ (error * 2654435761u));
  if (key == last_reported_key || event_source == nullptr) {
    return;
  }
  last_reported_key = key;

  auto wide = std::wstring(message.begin(), message.end());
  LPCWSTR strings[] = {wide.c_str()};
  ReportEventW(event_source, EVENTLOG_ERROR_TYPE, 0, 0, nullptr, 1, 0, strings, nullptr);
}

DWORD WINAPI HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
  switch (dwControl) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      // If a new session connects to the console, restart Sunshine
      // to allow it to spawn inside the new console session.
      if (dwEventType == WTS_CONSOLE_CONNECT) {
        SetEvent(session_change_event);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_PRESHUTDOWN:
      // The system is shutting down
    case SERVICE_CONTROL_STOP:
      // Let SCM know we're stopping in up to 30 seconds
      ReportStatus(SERVICE_STOP_PENDING, 0, STOP_WAIT_HINT_MS);

      // Trigger ServiceMain() to start cleanup
      SetEvent(stop_event);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

HANDLE CreateJobObjectForChildProcess() {
  HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
  if (!job_handle) {
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit_info = {};

  // Kill Sunshine.exe when the final job object handle is closed (which will happen if we terminate unexpectedly).
  // This ensures we don't leave an orphaned Sunshine.exe running with an inherited handle to our log file.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // Allow Sunshine.exe to use CREATE_BREAKAWAY_FROM_JOB when spawning processes to ensure they can to live beyond
  // the lifetime of SunshineSvc.exe. This avoids unexpected user data loss if we crash or are killed.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;

  if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &job_limit_info, sizeof(job_limit_info))) {
    CloseHandle(job_handle);
    return nullptr;
  }

  return job_handle;
}

LPPROC_THREAD_ATTRIBUTE_LIST AllocateProcThreadAttributeList(DWORD attribute_count) {
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);

  auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
  if (list == nullptr) {
    return nullptr;
  }

  if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
    HeapFree(GetProcessHeap(), 0, list);
    return nullptr;
  }

  return list;
}

void FreeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST list) {
  if (list != nullptr) {
    DeleteProcThreadAttributeList(list);
    HeapFree(GetProcessHeap(), 0, list);
  }
}

HANDLE DuplicateTokenForSession(DWORD console_session_id) {
  HANDLE current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current_token)) {
    return nullptr;
  }

  // Duplicate our own LocalSystem token
  HANDLE new_token;
  if (!DuplicateTokenEx(current_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &new_token)) {
    CloseHandle(current_token);
    return nullptr;
  }

  CloseHandle(current_token);

  // Change the duplicated token to the console session ID
  if (!SetTokenInformation(new_token, TokenSessionId, &console_session_id, sizeof(console_session_id))) {
    CloseHandle(new_token);
    return nullptr;
  }

  return new_token;
}

/**
 * @brief Absolute path of this executable, without the truncation limit of MAX_PATH.
 */
static std::wstring GetModulePath() {
  return svc::read_growing([](wchar_t *buffer, unsigned long size) {
    return (unsigned long) GetModuleFileNameW(nullptr, buffer, size);
  });
}

/**
 * @brief Create a fresh log file in the Temp folder (usually %SYSTEMROOT%\Temp).
 *
 * A previous log is kept as nyxplay-host.log.old (one generation) so the output of the last run
 * survives a restart of the service.
 */
HANDLE OpenLogFileHandle() {
  WCHAR temp_path[MAX_PATH + 1] = {};
  auto temp_len = GetTempPathW(_countof(temp_path), temp_path);
  if (temp_len == 0 || temp_len >= _countof(temp_path)) {
    return INVALID_HANDLE_VALUE;
  }

  std::wstring log_file_name = std::wstring(temp_path) + L"nyxplay-host.log";
  std::wstring old_log_file_name = log_file_name + L".old";

  // The file handle must be inheritable for our child process to use it.
  // %SystemRoot%\Temp is writable by every local user, so the log gets an explicit DACL
  // (SYSTEM and Administrators only) instead of the inherited one.
  SECURITY_ATTRIBUTES security_attributes = {sizeof(security_attributes), nullptr, TRUE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &security_attributes.lpSecurityDescriptor, nullptr)) {
    return INVALID_HANDLE_VALUE;
  }

  // Never reuse a file that already exists: a local user could have pre-created it with a DACL
  // that keeps them able to read what we log. Move it out of the way (replacing an older
  // generation) and create a fresh one exclusively (CREATE_NEW), without following a reparse point.
  // A few attempts cover a user re-creating the file between the move and the create.
  HANDLE log_file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 5 && log_file == INVALID_HANDLE_VALUE; attempt++) {
    if (!MoveFileExW(log_file_name.c_str(), old_log_file_name.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      // Nothing to keep or the file is busy: fall back to removing it
      DeleteFileW(log_file_name.c_str());
    }
    log_file = CreateFileW(log_file_name.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security_attributes, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  }

  if (log_file == INVALID_HANDLE_VALUE) {
    // The path is being held by someone else. Keep the service available but discard the log
    // rather than writing into a file we do not control.
    log_file = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security_attributes, OPEN_EXISTING, 0, nullptr);
  }

  LocalFree(security_attributes.lpSecurityDescriptor);
  return log_file;
}

bool RunTerminationHelper(HANDLE console_token, DWORD pid) {
  auto module_path = GetModulePath();
  if (module_path.empty()) {
    return false;
  }
  std::wstring command;

  command += L'"';
  command += module_path;
  command += L'"';
  command += std::format(L" --terminate {}", pid);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  // Execute ourselves as a detached process in the user session with the --terminate argument.
  // This will allow us to attach to Sunshine's console and send it a Ctrl-C event.
  PROCESS_INFORMATION process_info = {};
  if (!CreateProcessAsUserW(console_token, module_path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS, nullptr, nullptr, &startup_info, &process_info)) {
    return false;
  }

  // Wait for the termination helper to complete, but never indefinitely: a helper stuck in a
  // vanishing session must not keep the service in the stop pending state.
  if (WaitReportingProgress(process_info.hProcess, TERMINATION_HELPER_TIMEOUT_MS) != WAIT_OBJECT_0) {
    TerminateProcess(process_info.hProcess, ERROR_TIMEOUT);
    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
    return false;
  }

  // Check the exit status of the helper process
  DWORD exit_code = 1;
  GetExitCodeProcess(process_info.hProcess, &exit_code);

  // Cleanup handles
  CloseHandle(process_info.hProcess);
  CloseHandle(process_info.hThread);

  // If the helper process returned 0, it succeeded
  return exit_code == 0;
}

/**
 * @brief Whether process @p pid owns a listening TCP socket (IPv4 or IPv6).
 *
 * Returns true when the answer cannot be determined, so a failing query never causes a healthy host to be restarted.
 */
static bool HostIsListening(DWORD pid) {
  for (ULONG family : {(ULONG) AF_INET, (ULONG) AF_INET6}) {
    std::vector<BYTE> buffer(8192);
    ULONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; attempt++) {
      auto size = (ULONG) buffer.size();
      result = GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
      if (result == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(size + 4096);
      }
    }
    if (result != NO_ERROR) {
      return true;
    }

    if (family == AF_INET) {
      auto table = (PMIB_TCPTABLE_OWNER_PID) buffer.data();
      for (DWORD i = 0; i < table->dwNumEntries; i++) {
        if (table->table[i].dwOwningPid == pid) {
          return true;
        }
      }
    } else {
      auto table = (PMIB_TCP6TABLE_OWNER_PID) buffer.data();
      for (DWORD i = 0; i < table->dwNumEntries; i++) {
        if (table->table[i].dwOwningPid == pid) {
          return true;
        }
      }
    }
  }
  return false;
}

static std::wstring Widen(const char *text) {
  return std::wstring(text, text + strlen(text));
}

/**
 * @brief Read a small file completely; returns false if it is larger than @p max_bytes or cannot be read.
 */
static bool ReadSmallFile(const std::wstring &path, DWORD max_bytes, std::string &out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  out.assign(max_bytes + 1, '\0');
  DWORD got = 0;
  const bool ok = ReadFile(file, out.data(), max_bytes + 1, &got, nullptr) && got <= max_bytes;
  CloseHandle(file);
  out.resize(ok ? got : 0);
  return ok;
}

/**
 * @brief Lowercase hex SHA-256 of an open file, read from its current position to the end.
 */
static bool Sha256Hex(HANDLE file, std::string &hex) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
    std::vector<BYTE> buffer(64 * 1024);
    DWORD got = 0;
    ok = true;
    while (ReadFile(file, buffer.data(), (DWORD) buffer.size(), &got, nullptr) && got > 0) {
      if (BCryptHashData(hash, buffer.data(), got, 0) < 0) {
        ok = false;
        break;
      }
    }
    BYTE digest[32] = {};
    ok = ok && got == 0 && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (ok) {
      static const char digits[] = "0123456789abcdef";
      hex.clear();
      for (BYTE b : digest) {
        hex += digits[b >> 4];
        hex += digits[b & 15];
      }
    }
  }
  if (hash) {
    BCryptDestroyHash(hash);
  }
  if (algorithm) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  return ok;
}

static void WriteUpdateResult(const std::wstring &dir, DWORD code) {
  auto text = std::format("{}\n", code);
  HANDLE file = CreateFileW((dir + L"\\" + Widen(svc::update_result_file)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written;
    WriteFile(file, text.data(), (DWORD) text.size(), &written, nullptr);
    CloseHandle(file);
  }
}

/**
 * @brief Run the update installer that the host asked for.
 *
 * The request is only a file name and a hash. The file must be a plain file in the updates folder of this
 * installation (SYSTEM and Administrators only), is opened so that nobody can change it meanwhile, and must
 * hash to the value the host verified against the signed release list. The installer is started as a
 * process of its own, outside of the host's job, so that stopping this service does not end it.
 */
static void HandleUpdateRequest(HANDLE log_file) {
  if (update_process != nullptr) {
    LogLine(log_file, "update request ignored: an installer is already running");
    return;
  }

  auto install_dir = svc::strip_components(GetModulePath(), 2);
  if (install_dir.empty()) {
    return;
  }
  const std::wstring dir = install_dir + L"\\config\\updates";

  std::string text;
  svc::update_request_t request;
  if (!ReadSmallFile(dir + L"\\" + Widen(svc::update_request_file), 512, text) || !svc::parse_update_request(text, request)) {
    LogLine(log_file, "update request rejected: the request file is missing or malformed");
    WriteUpdateResult(dir, 3);
    return;
  }

  const std::wstring path = dir + L"\\" + std::wstring(request.name.begin(), request.name.end());
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    LogLine(log_file, std::format("update request rejected: the installer cannot be opened (error {})", GetLastError()));
    WriteUpdateResult(dir, 3);
    return;
  }

  BY_HANDLE_FILE_INFORMATION info = {};
  std::string actual;
  const bool plain = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks == 1 &&
                     ((((unsigned long long) info.nFileSizeHigh) << 32) | info.nFileSizeLow) <= svc::update_max_installer_bytes;
  if (!plain || !Sha256Hex(file, actual) || actual != request.sha256) {
    LogLine(log_file, "update request rejected: the installer is not the verified file");
    CloseHandle(file);
    WriteUpdateResult(dir, 3);
    return;
  }

  // The result file of an earlier run must not be mistaken for this one
  DeleteFileW((dir + L"\\" + Widen(svc::update_result_file)).c_str());

  std::wstring command = L"\"" + path + L"\" /S /UPDATE";
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  // The file stays open (no write, no delete by others) until the process exists
  const BOOL started = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, dir.c_str(), &startup_info, &process_info);
  const DWORD error = started ? NO_ERROR : GetLastError();
  CloseHandle(file);
  if (!started) {
    LogLine(log_file, std::format("update installer could not be started (error {})", error));
    WriteUpdateResult(dir, 4);
    return;
  }

  LogLine(log_file, std::format("update installer started (process {})", process_info.dwProcessId));
  CloseHandle(process_info.hThread);
  update_process = process_info.hProcess;
}

/**
 * @brief The update installer ended while this service is still running, so it did not replace the host.
 */
static void HandleInstallerExit(HANDLE log_file) {
  DWORD exit_code = 1;
  GetExitCodeProcess(update_process, &exit_code);
  CloseHandle(update_process);
  update_process = nullptr;
  LogLine(log_file, std::format("update installer ended with exit code {}", exit_code));
  auto install_dir = svc::strip_components(GetModulePath(), 2);
  if (!install_dir.empty()) {
    WriteUpdateResult(install_dir + L"\\config\\updates", exit_code);
  }
}

/**
 * @brief Tell SCM that the service failed to start and stopped.
 */
static void FailStartup(DWORD error) {
  ReportStatus(SERVICE_STOPPED, 0, 0, error != NO_ERROR ? error : ERROR_GEN_FAILURE);
}

VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv) {
  service_status_handle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, nullptr);
  if (service_status_handle == nullptr) {
    // Nothing we can really do here but terminate ourselves
    ExitProcess(GetLastError());
    return;
  }

  // Tell SCM we're starting
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwServiceSpecificExitCode = 0;
  ReportStatus(SERVICE_START_PENDING, 0, 10 * 1000);

  // Create a manual-reset stop event
  stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (stop_event == nullptr) {
    FailStartup(GetLastError());
    return;
  }

  // Create an auto-reset session change event
  session_change_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (session_change_event == nullptr) {
    FailStartup(GetLastError());
    return;
  }

  // Only SYSTEM (the host runs as SYSTEM) may ask for an update. Without the event updates are simply not offered.
  {
    SECURITY_ATTRIBUTES update_security = {sizeof(update_security), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &update_security.lpSecurityDescriptor, nullptr)) {
      update_event = CreateEventA(&update_security, FALSE, FALSE, svc::update_event_name);
      if (update_event != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(update_event);
        update_event = nullptr;
      }
      LocalFree(update_security.lpSecurityDescriptor);
    }
  }

  event_source = RegisterEventSourceW(nullptr, L"" SERVICE_NAME);

  auto log_file_handle = OpenLogFileHandle();
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    FailStartup(GetLastError());
    return;
  }

  // Start the host by absolute path next to the install directory of this executable
  // (<install dir>\tools\<this file>) instead of relying on the search path
  auto host_path = svc::strip_components(GetModulePath(), 2);
  if (host_path.empty()) {
    FailStartup(ERROR_INSUFFICIENT_BUFFER);
    return;
  }
  host_path += L"\\nyxplay-host.exe";

  LogLine(log_file_handle, "service started");

  // Tell SCM we're running (and stoppable now)
  ReportStatus(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE, 0);

  svc::restart_backoff_t backoff;
  svc::startup_watchdog_t watchdog;
  svc::loader_failure_reporter_t loader_failures;
  DWORD delay_ms = svc::backoff_initial_ms;
  DWORD last_reported_key = 0;

  // Loop until the stop event is set. The wait doubles as the pause between start attempts.
  while (WaitForSingleObject(stop_event, delay_ms) != WAIT_OBJECT_0) {
    // The pause for the next iteration is the base delay unless a failure below raises it
    delay_ms = svc::backoff_initial_ms;

    auto console_session_id = WTSGetActiveConsoleSessionId();
    if (console_session_id == 0xFFFFFFFF) {
      // No console session yet
      continue;
    }

    // Keep the log bounded: rotate between host runs, the handle is inherited per child
    LARGE_INTEGER log_size = {};
    if (GetFileSizeEx(log_file_handle, &log_size) && svc::should_rotate_log((std::uint64_t) log_size.QuadPart)) {
      CloseHandle(log_file_handle);
      log_file_handle = OpenLogFileHandle();
      if (log_file_handle == INVALID_HANDLE_VALUE) {
        FailStartup(GetLastError());
        return;
      }
    }

    auto console_token = DuplicateTokenForSession(console_session_id);
    if (console_token == nullptr) {
      ReportFailure(log_file_handle, "Creating the session token", GetLastError(), last_reported_key);
      delay_ms = backoff.on_stopped(0);
      continue;
    }

    // Job objects cannot span sessions, so we must create one for each process
    auto job_handle = CreateJobObjectForChildProcess();
    if (job_handle == nullptr) {
      ReportFailure(log_file_handle, "Creating the job object", GetLastError(), last_reported_key);
      CloseHandle(console_token);
      delay_ms = backoff.on_stopped(0);
      continue;
    }

    // The startup information is rebuilt for every child: the job and the (possibly rotated) log
    // handle differ per iteration
    STARTUPINFOEXW startup_info = {};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.StartupInfo.lpDesktop = (LPWSTR) L"winsta0\\default";
    startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_info.StartupInfo.hStdInput = nullptr;
    startup_info.StartupInfo.hStdOutput = log_file_handle;
    startup_info.StartupInfo.hStdError = log_file_handle;

    // Allocate an attribute list with space for 2 entries
    startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
    HANDLE inherit_handles[] = {log_file_handle};
    if (startup_info.lpAttributeList == nullptr ||
        // Only allow Sunshine.exe to inherit the log file handle, not all inheritable handles
        !UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_handles, sizeof(inherit_handles), nullptr, nullptr) ||
        // Start Sunshine.exe inside our job object
        !UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_handle, sizeof(job_handle), nullptr, nullptr)) {
      ReportFailure(log_file_handle, "Preparing the host process attributes", GetLastError(), last_reported_key);
      FreeProcThreadAttributeList(startup_info.lpAttributeList);
      CloseHandle(console_token);
      CloseHandle(job_handle);
      delay_ms = backoff.on_stopped(0);
      continue;
    }

    PROCESS_INFORMATION process_info = {};
    auto start_tick = GetTickCount64();
    if (!CreateProcessAsUserW(console_token, host_path.c_str(), nullptr, nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, (LPSTARTUPINFOW) &startup_info, &process_info)) {
      auto error = GetLastError();
      delay_ms = backoff.on_stopped(0);
      ReportFailure(log_file_handle, std::format("Starting the host (retry in {} s)", delay_ms / 1000), error, last_reported_key);
      FreeProcThreadAttributeList(startup_info.lpAttributeList);
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }
    FreeProcThreadAttributeList(startup_info.lpAttributeList);
    last_reported_key = 0;

    // Gracefully terminates the host, forcefully if it does not exit in time
    auto stop_host = [&]() {
      if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
          WaitReportingProgress(process_info.hProcess, HOST_EXIT_TIMEOUT_MS) != WAIT_OBJECT_0) {
        TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
      }
    };

    bool planned_restart = false;
    bool host_ready = false;
    bool still_running;
    do {
      // Wait for the stop event to be set, Sunshine.exe to terminate, or the console session to change.
      // Until the host listens on a port the wait is bounded so the startup watchdog can check on it.
      HANDLE wait_objects[5] = {stop_event, process_info.hProcess, session_change_event};
      DWORD wait_count = 3;
      const DWORD update_request_index = update_event != nullptr ? wait_count++ : MAXDWORD;
      if (update_event != nullptr) {
        wait_objects[update_request_index] = update_event;
      }
      const DWORD update_exit_index = update_process != nullptr ? wait_count++ : MAXDWORD;
      if (update_process != nullptr) {
        wait_objects[update_exit_index] = update_process;
      }
      const DWORD wait_result = WaitForMultipleObjects(wait_count, wait_objects, FALSE, host_ready ? INFINITE : HOST_READY_POLL_MS);
      if (update_request_index != MAXDWORD && wait_result == WAIT_OBJECT_0 + update_request_index) {
        HandleUpdateRequest(log_file_handle);
        still_running = true;
        continue;
      }
      if (update_exit_index != MAXDWORD && wait_result == WAIT_OBJECT_0 + update_exit_index) {
        HandleInstallerExit(log_file_handle);
        still_running = true;
        continue;
      }
      switch (wait_result) {
        case WAIT_TIMEOUT:
          {
            auto ran_ms = GetTickCount64() - start_tick;
            switch (watchdog.evaluate(ran_ms, HostIsListening(process_info.dwProcessId))) {
              case svc::host_state_t::ready:
                host_ready = true;
                watchdog.on_ready();
                break;
              case svc::host_state_t::stuck:
                // Alive but never reachable (a service started at boot can hang while the graphics driver,
                // network or desktop is not ready). Restart it instead of waiting forever.
                watchdog.on_stuck();
                stop_host();
                delay_ms = backoff.on_stopped(ran_ms);
                LogLine(log_file_handle, std::format("The host opened no listening port within {} s, restarting in {} s", ran_ms / 1000, delay_ms / 1000));
                still_running = false;
                continue;
              case svc::host_state_t::starting:
                break;
            }
            still_running = true;
            continue;
          }

        case WAIT_OBJECT_0 + 2:
          if (WTSGetActiveConsoleSessionId() == console_session_id) {
            // The active console session didn't actually change. Let Sunshine keep running.
            still_running = true;
            continue;
          }
          planned_restart = true;
          // Fall-through to terminate Sunshine.exe and start it again.
        case WAIT_OBJECT_0:
          // The service is shutting down, so try to gracefully terminate Sunshine.exe.
          // If it doesn't terminate in time, we will forcefully terminate it.
          stop_host();
          still_running = false;
          break;

        case WAIT_OBJECT_0 + 1:
          {
            // Sunshine terminated itself.

            DWORD exit_code = 0;
            if (GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == ERROR_SHUTDOWN_IN_PROGRESS) {
              // Sunshine is asking for us to shut down, so gracefully stop ourselves.
              SetEvent(stop_event);
            } else {
              auto ran_ms = GetTickCount64() - start_tick;
              delay_ms = backoff.on_stopped(ran_ms);
              LogLine(log_file_handle, std::format("The host exited with code {} after {} s, restarting in {} s", exit_code, ran_ms / 1000, delay_ms / 1000));
              if (auto cause = loader_failures.on_exit(exit_code, ran_ms)) {
                LogLine(log_file_handle, std::format("Cause: {} (exit code {:#010x})", cause, exit_code));
              }
            }
            still_running = false;
            break;
          }

        default:
          {
            // WAIT_FAILED: do not spin on a broken handle, end this run and back off
            ReportFailure(log_file_handle, "Waiting for the host", GetLastError(), last_reported_key);
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
            delay_ms = backoff.on_stopped(0);
            still_running = false;
            break;
          }
      }
    } while (still_running);

    if (planned_restart) {
      backoff.reset();
      loader_failures.reset();
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(console_token);
    CloseHandle(job_handle);
  }

  LogLine(log_file_handle, "service stopped");
  CloseHandle(log_file_handle);
  if (event_source != nullptr) {
    DeregisterEventSource(event_source);
  }
  CloseHandle(session_change_event);
  if (update_event != nullptr) {
    CloseHandle(update_event);
  }
  CloseHandle(stop_event);

  // Let SCM know we've stopped
  ReportStatus(SERVICE_STOPPED, 0, 0);
}

// This will run in a child process in the user session
int DoGracefulTermination(DWORD pid) {
  // Attach to Sunshine's console
  if (!AttachConsole(pid)) {
    return GetLastError();
  }

  // Disable our own Ctrl-C handling
  SetConsoleCtrlHandler(nullptr, TRUE);

  // Send a Ctrl-C event to Sunshine
  if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
    return GetLastError();
  }

  return 0;
}

int main(int argc, char *argv[]) {
  static const SERVICE_TABLE_ENTRY service_table[] = {
    {(LPSTR) SERVICE_NAME, ServiceMain},
    {nullptr, nullptr}
  };

  // Check if this is a reinvocation of ourselves to send Ctrl-C to Sunshine.exe
  if (argc == 3 && strcmp(argv[1], "--terminate") == 0) {
    return DoGracefulTermination(atol(argv[2]));
  }

  // By default, services have their current directory set to %SYSTEMROOT%\System32.
  // We want to use the directory where Sunshine.exe is located instead of system32.
  // This requires stripping off 2 path components: the file name and the last folder
  auto install_dir = svc::strip_components(GetModulePath(), 2);
  if (!install_dir.empty()) {
    SetCurrentDirectoryW(install_dir.c_str());
  }

  // Trigger our ServiceMain()
  return StartServiceCtrlDispatcher(service_table);
}
