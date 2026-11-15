/**
 * @file src/rtsp_validation.h
 * @brief Range checks for the values of an RTSP ANNOUNCE request.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>

namespace rtsp_stream {
  /// Highest video bitrate (kbps) accepted from a client, well above what any encoder supports.
  constexpr std::int64_t ANNOUNCE_MAX_BITRATE_KBPS = 1000000;

  /// Lowest video bitrate (kbps) used after the bitrate adjustments.
  constexpr std::int64_t ANNOUNCE_MIN_BITRATE_KBPS = 100;

  /// Values of the video related ANNOUNCE attributes, as sent by the client.
  struct announce_video_t {
    int width;
    int height;
    int framerate;  ///< 1..1000 is frames per second, above 4000 is millihertz
    std::int64_t bitrate;  ///< kbps
    int slicesPerFrame;
    int numRefFrames;
    int encoderCscMode;
    int videoFormat;
    int dynamicRange;
    int chromaSamplingType;
    int enableIntraRefresh;
  };

  /**
   * @brief Check the video attributes of an ANNOUNCE request.
   * @return true when every value is inside the range the encoders can work with.
   */
  inline bool announce_video_valid(const announce_video_t &v) {
    const bool framerate_ok = (v.framerate >= 1 && v.framerate <= 1000) || (v.framerate > 4000 && v.framerate <= 1000000);

    return v.width >= 128 && v.width <= 16384 &&
           v.height >= 72 && v.height <= 16384 &&
           framerate_ok &&
           v.bitrate >= 1 && v.bitrate <= ANNOUNCE_MAX_BITRATE_KBPS &&
           v.slicesPerFrame >= 1 && v.slicesPerFrame <= 32 &&
           v.numRefFrames >= 0 && v.numRefFrames <= 16 &&
           v.encoderCscMode >= 0 && v.encoderCscMode <= 7 &&
           v.videoFormat >= 0 && v.videoFormat <= 2 &&
           v.dynamicRange >= 0 && v.dynamicRange <= 1 &&
           v.chromaSamplingType >= 0 && v.chromaSamplingType <= 1 &&
           v.enableIntraRefresh >= 0 && v.enableIntraRefresh <= 1 &&
           // H.264 has no 10-bit profile that is streamed
           !(v.videoFormat == 0 && v.dynamicRange != 0);
  }

  /**
   * @brief Check the packet size and the FEC minimum of an ANNOUNCE request.
   */
  inline bool announce_stream_valid(int packetsize, int min_required_fec_packets) {
    return packetsize >= 200 && packetsize <= 65535 &&
           min_required_fec_packets >= 0 && min_required_fec_packets <= 16;
  }

  /**
   * @brief Bring a bitrate that went through the adjustments into the supported range.
   */
  inline std::int64_t clamp_bitrate_kbps(std::int64_t kbps) {
    return std::clamp(kbps, ANNOUNCE_MIN_BITRATE_KBPS, ANNOUNCE_MAX_BITRATE_KBPS);
  }
}  // namespace rtsp_stream
