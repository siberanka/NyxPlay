/**
 * @file tests/unit/test_host_hardening.cpp
 * @brief Test the small input handling limits of src/nvhttp.* and src/rtsp.*
 */
#include <string>
#include <string_view>

#include <src/nvhttp.h>
#include <src/rtsp.h>

#include "../tests_common.h"

using namespace std::literals;

TEST(OtpAttemptLimiter, OneAddressCannotUseUpTheOneTimePinOfOthers) {
  nvhttp::otp_attempt_limiter_t limiter;

  for (int i = 0; i < nvhttp::MAX_OTP_FAILURES; ++i) {
    EXPECT_TRUE(limiter.allowed("192.0.2.1")) << i;
    // The total cap is far away: the one-time PIN stays valid
    EXPECT_FALSE(limiter.record_failure("192.0.2.1")) << i;
  }

  EXPECT_FALSE(limiter.allowed("192.0.2.1"));
  EXPECT_TRUE(limiter.allowed("192.0.2.2"));
}

TEST(OtpAttemptLimiter, ManyAddressesHitTheTotalCap) {
  nvhttp::otp_attempt_limiter_t limiter;

  int failures = 0;
  bool invalidate = false;
  for (int address = 1; address < 100 && !invalidate; ++address) {
    const auto name = "198.51.100." + std::to_string(address);
    for (int i = 0; i < nvhttp::MAX_OTP_FAILURES && !invalidate; ++i) {
      ASSERT_TRUE(limiter.allowed(name));
      invalidate = limiter.record_failure(name);
      ++failures;
    }
  }

  EXPECT_TRUE(invalidate);
  EXPECT_EQ(failures, nvhttp::MAX_OTP_FAILURES_TOTAL);
}

TEST(OtpAttemptLimiter, ResetForgetsEverything) {
  nvhttp::otp_attempt_limiter_t limiter;
  for (int i = 0; i < nvhttp::MAX_OTP_FAILURES; ++i) {
    limiter.record_failure("192.0.2.1");
  }
  ASSERT_FALSE(limiter.allowed("192.0.2.1"));

  limiter.reset();
  EXPECT_TRUE(limiter.allowed("192.0.2.1"));
  for (int i = 0; i < nvhttp::MAX_OTP_FAILURES_TOTAL - 1; ++i) {
    EXPECT_FALSE(limiter.record_failure("203.0.113." + std::to_string(i)));
  }
}

TEST(RtspHeaderEnd, WholeMessageInOneRead) {
  const auto message = "OPTIONS rtsp://host RTSP/1.0\r\nCSeq: 1\r\n\r\n"sv;
  EXPECT_EQ(rtsp_stream::find_header_end(message, 0), message.size());
  EXPECT_EQ(rtsp_stream::find_header_end("OPTIONS rtsp://host RTSP/1.0\r\nCSeq: 1\r\n"sv, 0), std::string_view::npos);
}

TEST(RtspHeaderEnd, TerminatorSplitBetweenReads) {
  const auto message = "OPTIONS rtsp://host RTSP/1.0\r\nCSeq: 1\r\n\r\nbody"sv;
  const auto terminator_end = message.find("\r\n\r\n") + 4;

  // Every possible split of the terminator over two reads
  for (std::size_t first = message.find("\r\n\r\n") + 1; first < terminator_end; ++first) {
    EXPECT_EQ(rtsp_stream::find_header_end(message.substr(0, first), 0), std::string_view::npos) << first;
    EXPECT_EQ(rtsp_stream::find_header_end(message.substr(0, terminator_end), first), terminator_end) << first;
  }
}

TEST(RtspHeaderEnd, ByteByByteReads) {
  const auto message = "DESCRIBE rtsp://host RTSP/1.0\r\nCSeq: 2\r\n\r\n"sv;

  std::size_t searched = 0;
  std::size_t found = std::string_view::npos;
  for (std::size_t size = 1; size <= message.size() && found == std::string_view::npos; ++size) {
    found = rtsp_stream::find_header_end(message.substr(0, size), searched);
    searched = size;
  }
  EXPECT_EQ(found, message.size());
}

TEST(RtspHeaderEnd, OnlyNewDataIsSearchedButTheOverlap) {
  // A terminator that lies completely in the data that was searched before is not reported again
  EXPECT_EQ(rtsp_stream::find_header_end("abc\r\n\r\ndef"sv, 10), std::string_view::npos);
  EXPECT_EQ(rtsp_stream::find_header_end(""sv, 0), std::string_view::npos);
  EXPECT_EQ(rtsp_stream::find_header_end("\r\n\r\n"sv, 0), 4U);
  EXPECT_EQ(rtsp_stream::find_header_end("\r\n\r"sv, 0), std::string_view::npos);
}
