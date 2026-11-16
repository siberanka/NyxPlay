/**
 * @file tests/unit/test_rtsp_validation.cpp
 * @brief Test the ANNOUNCE range checks of src/rtsp_validation.h and the related video and audio helpers.
 */
#include <src/audio.h>
#include <src/rtsp_validation.h>
#include <src/utility.h>
#include <src/video.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>

#include "../tests_common.h"

namespace {
  using rtsp_stream::announce_video_t;
  using rtsp_stream::announce_video_valid;

  announce_video_t valid_video() {
    return {1920, 1080, 60, 20000, 1, 0, 1, 1, 0, 0, 0};
  }
}  // namespace

TEST(AnnounceValidation, VideoDimensions) {
  for (auto [w, h, ok] : {std::tuple {0, 0, false}, {-1, -1, false}, {1, 1, false}, {127, 71, false}, {128, 72, true}, {1920, 1080, true}, {7680, 4320, true}, {16384, 16384, true}, {16385, 1080, false}, {1920, 16385, false}}) {
    auto v = valid_video();
    v.width = w;
    v.height = h;
    EXPECT_EQ(announce_video_valid(v), ok) << w << 'x' << h;
  }
}

TEST(AnnounceValidation, Framerate) {
  for (auto [fps, ok] : {std::pair {0, false}, {-1, false}, {1, true}, {60, true}, {120, true}, {1000, true}, {1001, false}, {3999, false}, {4000, false}, {4001, true}, {59940, true}, {1000000, true}, {1000001, false}}) {
    auto v = valid_video();
    v.framerate = fps;
    EXPECT_EQ(announce_video_valid(v), ok) << fps;
  }
}

TEST(AnnounceValidation, Bitrate) {
  for (auto [kbps, ok] : {std::pair<std::int64_t, bool> {0, false}, {-5, false}, {1, true}, {500, true}, {150000, true}, {1000000, true}, {1000001, false}, {10000000, false}, {std::numeric_limits<std::int64_t>::max(), false}}) {
    auto v = valid_video();
    v.bitrate = kbps;
    EXPECT_EQ(announce_video_valid(v), ok) << kbps;
  }
}

TEST(AnnounceValidation, EnumRanges) {
  auto check = [](auto mutate, bool ok) {
    auto v = valid_video();
    mutate(v);
    EXPECT_EQ(announce_video_valid(v), ok);
  };

  check([](auto &v) { v.slicesPerFrame = 0; }, false);
  check([](auto &v) { v.slicesPerFrame = -1; }, false);
  check([](auto &v) { v.slicesPerFrame = 33; }, false);
  check([](auto &v) { v.slicesPerFrame = 32; }, true);
  check([](auto &v) { v.numRefFrames = -1; }, false);
  check([](auto &v) { v.numRefFrames = 17; }, false);
  check([](auto &v) { v.numRefFrames = 16; }, true);
  check([](auto &v) { v.encoderCscMode = 8; }, false);
  check([](auto &v) { v.encoderCscMode = 7; }, true);
  check([](auto &v) { v.videoFormat = 3; }, false);
  check([](auto &v) { v.videoFormat = -1; }, false);
  check([](auto &v) { v.videoFormat = 2; }, true);
  check([](auto &v) { v.dynamicRange = 2; }, false);
  check([](auto &v) { v.chromaSamplingType = 2; }, false);
  check([](auto &v) { v.enableIntraRefresh = 2; }, false);
  check([](auto &v) { v.videoFormat = 0; v.dynamicRange = 1; }, false);
  check([](auto &v) { v.videoFormat = 1; v.dynamicRange = 1; }, true);
}

TEST(AnnounceValidation, PacketSizeAndFec) {
  using rtsp_stream::announce_stream_valid;
  EXPECT_FALSE(announce_stream_valid(199, 0));
  EXPECT_FALSE(announce_stream_valid(65536, 0));
  EXPECT_TRUE(announce_stream_valid(200, 0));
  EXPECT_TRUE(announce_stream_valid(1392, 2));
  EXPECT_TRUE(announce_stream_valid(1392, 16));
  EXPECT_FALSE(announce_stream_valid(1392, 17));
  EXPECT_FALSE(announce_stream_valid(1392, -1));
}

TEST(AnnounceValidation, BitrateAfterAdjustmentStaysInRange) {
  using rtsp_stream::clamp_bitrate_kbps;
  EXPECT_EQ(clamp_bitrate_kbps(0), rtsp_stream::ANNOUNCE_MIN_BITRATE_KBPS);
  EXPECT_EQ(clamp_bitrate_kbps(-4), rtsp_stream::ANNOUNCE_MIN_BITRATE_KBPS);
  EXPECT_EQ(clamp_bitrate_kbps(20000), 20000);
  EXPECT_EQ(clamp_bitrate_kbps(1000000LL * 60), rtsp_stream::ANNOUNCE_MAX_BITRATE_KBPS);
}

TEST(VideoBitrate, NoOverflow) {
  EXPECT_EQ(video::bitrate_bps(1), 1000);
  EXPECT_EQ(video::bitrate_bps(150000), 150000000);
  EXPECT_EQ(video::bitrate_bps(video::MAX_BITRATE_KBPS), 1000000000);
  EXPECT_EQ(video::bitrate_bps(2147483), 2147483000);
  EXPECT_EQ(video::bitrate_bps(2147484), std::numeric_limits<int>::max());
  EXPECT_EQ(video::bitrate_bps(10000000), std::numeric_limits<int>::max());
  EXPECT_EQ(video::bitrate_bps(-1), 0);
  // The announced cap is the one the encoders are limited by
  EXPECT_EQ(rtsp_stream::ANNOUNCE_MAX_BITRATE_KBPS, video::MAX_BITRATE_KBPS);
}

TEST(AudioParams, CustomSurroundRejectsCoupledGreaterThanStreams) {
  auto params = [](int channels, int streams, int coupled, std::initializer_list<std::uint8_t> map) {
    audio::stream_params_t p {channels, streams, coupled, {0}};
    int i = 0;
    for (auto m : map) {
      p.mapping[i++] = m;
    }
    return p;
  };

  EXPECT_FALSE(audio::stream_params_valid(params(6, 0, 6, {0, 1, 2, 3, 4, 5})));
  EXPECT_TRUE(audio::stream_params_valid(params(6, 4, 2, {0, 1, 4, 5, 2, 3})));
  EXPECT_TRUE(audio::stream_params_valid(params(8, 5, 3, {0, 1, 2, 3, 4, 5, 6, 7})));
  EXPECT_TRUE(audio::stream_params_valid(params(8, 8, 0, {0, 1, 2, 3, 4, 5, 6, 7})));
  EXPECT_FALSE(audio::stream_params_valid(params(6, 2, 4, {0, 1, 2, 3, 4, 5})));
  EXPECT_FALSE(audio::stream_params_valid(params(6, 4, 2, {0, 1, 2, 3, 4, 6})));
  EXPECT_FALSE(audio::stream_params_valid(params(4, 2, 2, {0, 1, 2, 3})));
}

TEST(AudioParams, PacketDurationAllowList) {
  for (int d : {1, 2, 3, 4, 6, 7, 11, 100, -5, 0}) {
    EXPECT_FALSE(audio::packet_duration_valid(d)) << d;
  }
  for (int d : {5, 10, 20, 40, 60}) {
    EXPECT_TRUE(audio::packet_duration_valid(d)) << d;
  }
}

TEST(AudioParams, ChannelAllowList) {
  for (int c : {0, 1, 3, 4, 5, 7, 9}) {
    EXPECT_FALSE(audio::channel_count_valid(c)) << c;
  }
  for (int c : {2, 6, 8}) {
    EXPECT_TRUE(audio::channel_count_valid(c)) << c;
  }
}

TEST(AudioParams, DefaultConfigsFitThePacketBuffer) {
  // The sizes the common clients use must stay valid
  for (int channels : {2, 6, 8}) {
    for (bool hq : {false, true}) {
      audio::config_t config {};
      config.channels = channels;
      config.packetDuration = 5;
      config.flags[audio::config_t::HIGH_QUALITY] = hq;
      EXPECT_TRUE(audio::config_valid(config)) << channels << ' ' << hq;
    }
  }

  audio::config_t config {};
  config.channels = 8;
  config.packetDuration = 60;
  config.flags[audio::config_t::HIGH_QUALITY] = true;
  EXPECT_FALSE(audio::config_valid(config));
}

TEST(AudioParams, InvalidCustomParamsAreRejectedByConfig) {
  audio::config_t config {};
  config.channels = 6;
  config.packetDuration = 5;
  config.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = true;
  config.customStreamParams = {6, 0, 6, {0, 1, 2, 3, 4, 5}};
  EXPECT_FALSE(audio::config_valid(config));
}

TEST(IntegerParsing, PlainValues) {
  EXPECT_EQ(util::from_view(""), 0);
  EXPECT_EQ(util::from_view("0"), 0);
  EXPECT_EQ(util::from_view("1920"), 1920);
  EXPECT_EQ(util::from_view("-42"), -42);
  EXPECT_EQ(util::from_view("9223372036854775807"), std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(util::from_view("-9223372036854775808"), std::numeric_limits<std::int64_t>::min());
}

TEST(IntegerParsing, OverflowIsClamped) {
  EXPECT_EQ(util::from_view("9223372036854775808"), std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(util::from_view("99999999999999999999"), std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(util::from_view("-99999999999999999999"), std::numeric_limits<std::int64_t>::min());
}

TEST(IntegerParsing, StopsAtTheFirstNonDigit) {
  EXPECT_EQ(util::from_view("12abc"), 12);
  EXPECT_EQ(util::from_view("abc"), 0);
}

TEST(IntegerParsing, NarrowingRejectsOutOfRangeValues) {
  EXPECT_EQ(util::from_view_as<int>("1920"), 1920);
  EXPECT_EQ(util::from_view_as<int>("-2147483648"), std::numeric_limits<int>::min());
  EXPECT_EQ(util::from_view_as<int>("2147483647"), std::numeric_limits<int>::max());
  // 2^32 + 1920 would wrap to a valid width
  EXPECT_THROW(util::from_view_as<int>("4294969216"), std::out_of_range);
  EXPECT_THROW(util::from_view_as<int>("2147483648"), std::out_of_range);
  EXPECT_THROW(util::from_view_as<int>("99999999999999999999"), std::out_of_range);
}
