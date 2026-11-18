/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <memory>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/asio/ip/address.hpp>

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

    /// Normalised address of the client that called /launch or /resume. Unencrypted RTSP is only served to it.
    std::string remote_address;

    /// Highest sequence number accepted on an encrypted RTSP message of this launch.
    std::atomic<std::uint32_t> rtsp_last_sequence {0};

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
   * @brief Textual form of a peer address used to compare the peers of a launch.
   * @details IPv4-mapped IPv6 addresses become IPv4 addresses and the scope id of a link-local address is dropped.
   */
  std::string peer_address_key(const boost::asio::ip::address &address);

  /**
   * @brief Key used to count the connections of a peer.
   * @details Like peer_address_key(), but IPv6 addresses are grouped by their /64 prefix, a single host or
   *          subscriber usually controls the whole prefix.
   */
  std::string peer_group_key(const boost::asio::ip::address &address);

  /**
   * @brief Bounds the number of open RTSP connections, in total and per peer group.
   * @details The count is taken when a connection is attached to a pending launch and returned when the
   *          connection object is destroyed, so every close path gives the slot back.
   */
  class connection_limiter_t {
  public:
    connection_limiter_t(int max_total, int max_per_peer, int max_per_launch_peer):
        _max_total {max_total},
        _max_per_peer {max_per_peer},
        _max_per_launch_peer {max_per_launch_peer} {
    }

    /**
     * @param group_key Result of peer_group_key().
     * @param launching_peer true when the peer is the client of the pending launch, it gets a larger allowance.
     * @return true when the connection was counted.
     */
    bool try_acquire(const std::string &group_key, bool launching_peer);

    void release(const std::string &group_key);

    int total() const;

    int count(const std::string &group_key) const;

  private:
    mutable std::mutex _mutex;
    std::map<std::string, int> _per_peer;
    int _total = 0;
    int _max_total;
    int _max_per_peer;
    int _max_per_launch_peer;
  };

  /**
   * @brief Check whether a peer may speak RTSP for the launch session.
   * @details An encrypted launch is not bound to an address: every message has to carry a valid tag under the key of
   *          the launch, which only the launching client has. An unencrypted launch is only served to the address
   *          that called /launch, a launch that has no recorded address is served to nobody.
   */
  bool rtsp_peer_allowed(const launch_session_t &launch_session, const boost::asio::ip::address &peer);

  /**
   * @brief Check the sequence number of an authenticated encrypted RTSP message.
   * @details The number has to be larger than every number accepted before for the launch, a captured message
   *          can not be delivered a second time. The number is recorded when it is accepted.
   */
  bool rtsp_accept_sequence(launch_session_t &launch_session, std::uint32_t sequence);

  /**
   * @brief Check whether the rtsp_plaintext option allows an unencrypted launch for the client address.
   * @param mode One of config::RTSP_PLAINTEXT_*.
   */
  bool plaintext_rtsp_allowed(int mode, const boost::asio::ip::address &client);

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
   * @brief Stop the streams of an address group and discard a launch it left pending.
   * @param group_key Key from security_log::group_key(), i.e. an IPv4 address or an IPv6 /64.
   */
  void stop_sessions_from_group(const std::string &group_key);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
