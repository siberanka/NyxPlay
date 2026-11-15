/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <memory>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "crypto.h"
#include "thread_safe.h"

#ifdef _WIN32
  #include <windows.h>
#endif

// Resolve circular dependencies
namespace stream {
  struct session_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    std::string device_name;
    std::string unique_id;
    crypto::PERM perm;

    bool input_only;
    bool host_audio;
    int width;
    int height;
    int fps;
    int gcmap;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool virtual_display;
    uint32_t scale_factor;
    bool text_input_events;  ///< NyxPlay: client wants text input focus notifications

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    /// Set by the first accepted ANNOUNCE. A launch session runs one stream at a time, a repeated ANNOUNCE
    /// replaces the stream as long as the control connection has not been made.
    std::atomic<bool> announced {false};

    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;

  #ifdef _WIN32
    GUID display_guid{};
  #endif
  };

  /**
   * @brief Holds the launch session that waits for its RTSP handshake.
   * @details One launch can be pending at a time. A new launch of the same client replaces it, a launch of
   *          another client is refused until the pending one connected or expired.
   */
  class launch_slot_t {
  public:
    enum class result_e {
      raised,  ///< The slot was empty
      replaced,  ///< A pending launch of the same client was replaced
      busy,  ///< Another client's launch is pending
    };

    struct raise_result_t {
      result_e result;
      std::shared_ptr<launch_session_t> replaced;  ///< The launch that was replaced
    };

    raise_result_t raise(std::shared_ptr<launch_session_t> launch_session);

    std::shared_ptr<launch_session_t> pending() const;

    /// @return true when the launch with the id was pending and has been removed.
    bool clear(uint32_t launch_session_id);

    /// Remove the pending launch of the client, if any.
    std::shared_ptr<launch_session_t> drop(std::string_view uuid);

    /// Remove the pending launch whoever it belongs to.
    std::shared_ptr<launch_session_t> drop_any();

    /// @return true when a launch of another client is pending.
    bool blocked_for(std::string_view uuid) const;

    /// @return true when the caller is the first to claim the ANNOUNCE of the launch session.
    static bool claim_announce(launch_session_t &launch_session);

    static void release_announce(launch_session_t &launch_session);

  private:
    mutable std::mutex _mutex;
    std::shared_ptr<launch_session_t> _pending;
  };

  /**
   * @brief Queue a launch session for the RTSP handshake.
   * @details A pending launch of the same client is replaced (and its not yet connected stream stopped).
   *          The pending window is `ping_timeout`, which is also the initial ping timeout of a stream.
   * @return false when the launch of another client is pending.
   */
  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /// @return true when the launch of another client is waiting for its RTSP handshake.
  bool launch_session_blocked_for(const std::string &uuid);

  /// Discard a pending launch of the client and stop the not yet connected streams created from it.
  void launch_session_drop(const std::string &uuid);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  std::shared_ptr<stream::session_t>
  find_session(const std::string_view& uuid);

  /// All live sessions of the client.
  std::vector<std::shared_ptr<stream::session_t>>
  find_sessions(const std::string_view& uuid);

  std::list<std::string>
  get_all_session_uuids();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
