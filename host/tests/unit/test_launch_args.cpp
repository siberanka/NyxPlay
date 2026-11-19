/**
 * @file tests/unit/test_launch_args.cpp
 * @brief Test the validation of the launch arguments of clients and the client command policy of src/nvhttp.* and src/process.*
 */
#include <src/nvhttp.h>
#include <src/process.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "../tests_common.h"

namespace {
  crypto::named_cert_t make_client() {
    crypto::named_cert_t client {};
    client.uuid = "77777777-7777-7777-7777-777777777777";
    client.perm = crypto::PERM::_all;
    return client;
  }

  nvhttp::args_t valid_args() {
    nvhttp::args_t args;
    args.emplace("rikey", "00112233445566778899aabbccddeeff");
    args.emplace("rikeyid", "1");
    return args;
  }

  nvhttp::args_t with(nvhttp::args_t args, const std::string &name, const std::string &value) {
    args.erase(name);
    args.emplace(name, value);
    return args;
  }

  /// @return The reason the launch is rejected with, empty if it is accepted.
  std::string rejection(const nvhttp::args_t &args) {
    auto client = make_client();
    try {
      nvhttp::make_launch_session(false, false, args, &client);
    } catch (const std::invalid_argument &e) {
      return e.what();
    }
    return {};
  }

  std::shared_ptr<rtsp_stream::launch_session_t> session_with_commands() {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->client_do_cmds.push_back({"do-cmd", true});
    session->client_undo_cmds.push_back({"undo-cmd", true});
    return session;
  }
}  // namespace

TEST(ClientCommandPolicy, KeptOnlyWhenAppAndDeviceAllow) {
  for (const bool app_allows : {false, true}) {
    for (const bool device_allows : {false, true}) {
      auto session = session_with_commands();
      nvhttp::restrict_client_commands(*session, app_allows, device_allows);
      const bool kept = app_allows && device_allows;
      EXPECT_EQ(session->client_do_cmds.size(), kept ? 1U : 0U) << "app " << app_allows << " device " << device_allows;
      EXPECT_EQ(session->client_undo_cmds.size(), kept ? 1U : 0U) << "app " << app_allows << " device " << device_allows;
    }
  }
}

TEST(ScaleFactor, ValidRangeIsAccepted) {
  EXPECT_EQ(rejection(valid_args()), "");
  for (const char *value : {"20", "100", "200", "400"}) {
    EXPECT_EQ(rejection(with(valid_args(), "scaleFactor", value)), "") << value;
  }

  auto client = make_client();
  auto session = nvhttp::make_launch_session(false, false, with(valid_args(), "scaleFactor", "150"), &client);
  EXPECT_EQ(session->scale_factor, 150U);
  session = nvhttp::make_launch_session(false, false, valid_args(), &client);
  EXPECT_EQ(session->scale_factor, 100U);
}

TEST(ScaleFactor, OutOfRangeIsRejected) {
  for (const char *value : {"-5", "0", "19", "401", "100000", "99999999999999999999", "abc", ""}) {
    EXPECT_EQ(rejection(with(valid_args(), "scaleFactor", value)), "Invalid scaleFactor") << value;
  }
}

TEST(ScaleFactor, RenderSizeIsScaledAndBounded) {
  using size_t = std::pair<std::uint32_t, std::uint32_t>;

  EXPECT_EQ(proc::scaled_render_size(1921, 1081, 100), (size_t {1921, 1081}));
  EXPECT_EQ(proc::scaled_render_size(1920, 1080, 200), (size_t {3840, 2160}));
  EXPECT_EQ(proc::scaled_render_size(1921, 1081, 150), (size_t {2881 & ~1U, 1621 & ~1U}));
  EXPECT_EQ(proc::scaled_render_size(1920, 1080, 20), (size_t {384, 216}));
  EXPECT_EQ(proc::scaled_render_size(4096, 2160, 400), (size_t {16384, 8640}));

  // The scaled size must stay in 1..16384
  EXPECT_FALSE(proc::scaled_render_size(4097, 2160, 400));
  EXPECT_FALSE(proc::scaled_render_size(16384, 1080, 101));
  EXPECT_FALSE(proc::scaled_render_size(1, 1, 20));
  EXPECT_FALSE(proc::scaled_render_size(3, 3, 20));
  EXPECT_FALSE(proc::scaled_render_size(1920, 1080, 19));
  EXPECT_FALSE(proc::scaled_render_size(1920, 1080, 401));
  EXPECT_FALSE(proc::scaled_render_size(1920, 1080, -100));
  EXPECT_FALSE(proc::scaled_render_size(1920, 1080, 0));
  EXPECT_FALSE(proc::scaled_render_size(0xFFFFFFFFU, 0xFFFFFFFFU, 400));
  EXPECT_TRUE(proc::scaled_render_size(16384, 16384, 100));
  EXPECT_FALSE(proc::scaled_render_size(16385, 1080, 100));
}

TEST(ScaleFactor, AdminConfigurationIsClamped) {
  EXPECT_EQ(proc::clamp_scale_factor(100), 100);
  EXPECT_EQ(proc::clamp_scale_factor(-1), proc::min_scale_factor);
  EXPECT_EQ(proc::clamp_scale_factor(0), proc::min_scale_factor);
  EXPECT_EQ(proc::clamp_scale_factor(1000000), proc::max_scale_factor);
}

TEST(LaunchArguments, StreamOptionsAreBounded) {
  EXPECT_EQ(rejection(with(valid_args(), "sops", "1")), "");
  EXPECT_EQ(rejection(with(valid_args(), "hdrMode", "1")), "");
  EXPECT_EQ(rejection(with(valid_args(), "gcmap", "65535")), "");
  EXPECT_EQ(rejection(with(valid_args(), "surroundAudioInfo", "393224")), "");  // 7.1: mask 0x6, 8 channels
  EXPECT_EQ(rejection(with(valid_args(), "surroundAudioInfo", "196610")), "");
  // What the clients send for 5.1 (mask 0x3F, 6 channels) and 7.1 (mask 0x63F, 8 channels)
  EXPECT_EQ(rejection(with(valid_args(), "surroundAudioInfo", "4128774")), "");
  EXPECT_EQ(rejection(with(valid_args(), "surroundAudioInfo", "104792072")), "");

  for (const char *value : {"2", "-1", "99999999999", "x"}) {
    EXPECT_EQ(rejection(with(valid_args(), "sops", value)), value == std::string {"x"} ? "" : "Invalid stream options") << value;
    EXPECT_EQ(rejection(with(valid_args(), "hdrMode", value)), value == std::string {"x"} ? "" : "Invalid stream options") << value;
  }
  for (const char *value : {"-1", "65536", "99999999999"}) {
    EXPECT_EQ(rejection(with(valid_args(), "gcmap", value)), "Invalid stream options") << value;
  }
  for (const char *value : {"-1", "0", "9", "196608", "2147483648", "99999999999"}) {
    EXPECT_EQ(rejection(with(valid_args(), "surroundAudioInfo", value)), "Invalid stream options") << value;
  }
}

TEST(LaunchArguments, RikeyIsExactlyThirtyTwoHexDigits) {
  EXPECT_EQ(rejection(with(valid_args(), "rikey", "00112233445566778899AABBCCDDEEFF")), "");
  for (const char *value : {"", "abcd", "00112233445566778899aabbccddeef", "00112233445566778899aabbccddeeff0", "zz112233445566778899aabbccddeeff", "00112233445566778899aabbccddeef-", "0011223344556677 899aabbccddeeff"}) {
    EXPECT_EQ(rejection(with(valid_args(), "rikey", value)), "Invalid rikey") << value;
  }
}
