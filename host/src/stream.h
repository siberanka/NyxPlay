/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace fec {
    /// Parity shard count of a block and the FEC percentage announced to the client.
    struct parity_plan_t {
      size_t parity_shards;
      size_t percentage;
    };

    parity_plan_t plan_parity(size_t data_shards, size_t fecpercentage, size_t minparityshards);
  }  // namespace fec

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    bool uuid_match(const session_t& session, const std::string_view& uuid);

    /// Id of the launch session the stream was created from.
    std::uint32_t launch_id(const session_t &session);

    /// Forget the client do/undo commands, another stream took them over.
    void drop_client_commands(session_t &session);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    /// Remember why the stream ends (the first reason wins), for the security log.
    void note_end_reason(session_t &session, const char *reason);
    /// Normalised address of the client the stream was started for.
    std::string remote_address(const session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);
  }  // namespace session
}  // namespace stream
