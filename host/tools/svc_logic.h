/**
 * @file tools/svc_logic.h
 * @brief Platform independent helpers of the Windows service (restart backoff, log rotation, path buffers).
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace svc {

  /// Delay before the first restart of the host and after a run that lasted long enough.
  constexpr std::uint32_t backoff_initial_ms = 3000;
  /// Upper limit of the restart delay.
  constexpr std::uint32_t backoff_max_ms = 60000;
  /// A host that stayed up at least this long is considered healthy and resets the backoff.
  constexpr std::uint64_t backoff_healthy_run_ms = 60000;
  /// The log file is rotated before a start when it is larger than this.
  constexpr std::uint64_t log_rotate_bytes = 10ull * 1024 * 1024;
  /// A host that has opened no listening port this long after its start is considered stuck and is restarted.
  constexpr std::uint64_t ready_grace_initial_ms = 3 * 60 * 1000;
  /// Upper limit of that period; it grows after every stuck run so a genuinely slow start is not restarted forever.
  constexpr std::uint64_t ready_grace_max_ms = 10 * 60 * 1000;

  /**
   * @brief Exponential restart backoff: 3 s, 6 s, 12 s ... capped at 60 s.
   */
  class restart_backoff_t {
  public:
    /**
     * @brief Report that the host stopped (or failed to start) after running for @p ran_ms.
     * @return The delay in milliseconds to wait before the next start attempt.
     */
    std::uint32_t on_stopped(std::uint64_t ran_ms) {
      if (ran_ms >= backoff_healthy_run_ms) {
        delay = backoff_initial_ms;
      }
      auto result = delay;
      delay = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(delay) * 2, backoff_max_ms));
      return result;
    }

    /**
     * @brief Forget previous failures (planned restart, session change).
     */
    void reset() {
      delay = backoff_initial_ms;
    }

  private:
    std::uint32_t delay = backoff_initial_ms;
  };

  /// State of a running host as seen by the startup watchdog.
  enum class host_state_t {
    starting,  ///< No listening port yet, still within the grace period.
    ready,  ///< The host listens on at least one port.
    stuck  ///< No listening port after the grace period.
  };

  /**
   * @brief Detects a host that is alive but never became reachable (for example hung while the graphics driver,
   *        network or desktop was not ready at boot). The grace period doubles after each stuck run.
   */
  class startup_watchdog_t {
  public:
    /**
     * @brief Classify a host that has been running for @p ran_ms.
     * @param listening Whether the host process owns a listening TCP socket.
     */
    host_state_t evaluate(std::uint64_t ran_ms, bool listening) const {
      if (listening) {
        return host_state_t::ready;
      }
      return ran_ms >= grace ? host_state_t::stuck : host_state_t::starting;
    }

    /**
     * @brief The host became reachable: the next run gets the initial grace period again.
     */
    void on_ready() {
      grace = ready_grace_initial_ms;
    }

    /**
     * @brief The host was stuck: allow the next run more time.
     */
    void on_stuck() {
      grace = std::min<std::uint64_t>(grace * 2, ready_grace_max_ms);
    }

    /**
     * @brief Grace period applied to the current run.
     */
    std::uint64_t grace_ms() const {
      return grace;
    }

  private:
    std::uint64_t grace = ready_grace_initial_ms;
  };

  /// Exit codes the Windows loader gives a process that could not even start.
  constexpr std::uint32_t status_dll_not_found = 0xC0000135u;
  constexpr std::uint32_t status_entrypoint_not_found = 0xC0000139u;
  constexpr std::uint32_t status_invalid_image_format = 0xC000007Bu;

  /**
   * @brief Plain-words cause for an exit code that means the host executable failed to load.
   * @return The explanation, or nullptr when @p exit_code is not a loader failure.
   */
  constexpr const char *describe_loader_failure(std::uint32_t exit_code) {
    switch (exit_code) {
      case status_dll_not_found:
        return "the host executable could not start because a required DLL is missing or damaged; reinstall NyxPlay Host";
      case status_entrypoint_not_found:
        return "the host executable could not start because a required DLL has the wrong version; reinstall NyxPlay Host";
      case status_invalid_image_format:
        return "the host executable could not start because it or a required DLL is damaged or built for another architecture; reinstall NyxPlay Host";
      default:
        return nullptr;
    }
  }

  /**
   * @brief Names a loader failure once per series of consecutive failures, so the restart loop does not
   *        repeat the same explanation every few seconds.
   */
  class loader_failure_reporter_t {
  public:
    /**
     * @brief Report that the host exited with @p exit_code after running for @p ran_ms.
     * @return The explanation to log, or nullptr when there is nothing new to say.
     */
    const char *on_exit(std::uint32_t exit_code, std::uint64_t ran_ms) {
      auto message = describe_loader_failure(exit_code);
      if (message == nullptr || ran_ms >= backoff_healthy_run_ms) {
        reported = false;
        return nullptr;
      }
      if (reported) {
        return nullptr;
      }
      reported = true;
      return message;
    }

    /**
     * @brief Start a new series (planned restart, successful run).
     */
    void reset() {
      reported = false;
    }

  private:
    bool reported = false;
  };

  /**
   * @brief Whether a log file of @p size bytes has to be rotated.
   */
  constexpr bool should_rotate_log(std::uint64_t size) {
    return size > log_rotate_bytes;
  }

  /**
   * @brief Read a string through an API that follows the GetModuleFileName contract:
   *        it returns the number of characters copied and, when the buffer was too small,
   *        returns the buffer size (truncated result). The buffer grows until the result fits.
   * @param fn Callable `unsigned long(wchar_t *buffer, unsigned long size)`; 0 means failure.
   * @return The string, or an empty string on failure or when the length exceeds 32767 characters.
   */
  template<typename Fn>
  std::wstring read_growing(Fn &&fn) {
    std::wstring buffer(260, L'\0');
    for (;;) {
      auto size = static_cast<unsigned long>(buffer.size());
      auto len = fn(buffer.data(), size);
      if (len == 0) {
        return {};
      }
      if (len < size) {
        buffer.resize(len);
        return buffer;
      }
      if (buffer.size() >= 32768) {
        return {};
      }
      buffer.resize(buffer.size() * 2);
    }
  }

  /**
   * @brief Remove the last @p count path components (file name included) from @p path.
   */
  inline std::wstring strip_components(std::wstring path, int count) {
    for (int i = 0; i < count; i++) {
      auto pos = path.find_last_of(L'\\');
      if (pos == std::wstring::npos) {
        break;
      }
      path.resize(pos);
    }
    return path;
  }

}  // namespace svc
