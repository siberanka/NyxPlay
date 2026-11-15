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
