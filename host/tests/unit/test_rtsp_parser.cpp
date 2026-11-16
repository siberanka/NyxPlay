/**
 * @file tests/unit/test_rtsp_parser.cpp
 * @brief Test the RTSP message parser built from src/rtsp_parser.c.
 */
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

#include <string>
#include <string_view>
#include <vector>

#include "../tests_common.h"

namespace {
  // Parses text from a buffer of exactly its length, so a read past the message ends outside the input.
  int parse(std::string_view text, RTSP_MESSAGE &msg) {
    std::vector<char> buffer(text.begin(), text.end());
    msg = {};
    return parseRtspMessage(&msg, buffer.data(), (int) buffer.size());
  }

  std::string option(RTSP_MESSAGE &msg, const char *name) {
    for (auto opt = msg.options; opt; opt = opt->next) {
      if (opt->option && std::string_view {opt->option} == name) {
        return opt->content ? opt->content : "";
      }
    }
    return {};
  }
}  // namespace

TEST(RtspParser, RequestWithEndMarker) {
  RTSP_MESSAGE msg;
  ASSERT_EQ(parse("OPTIONS rtsp://10.0.0.1:48010 RTSP/1.0\r\nCSeq: 1\r\nX-GS-ClientVersion: 14\r\n\r\n", msg), RTSP_ERROR_SUCCESS);
  EXPECT_EQ(msg.type, TYPE_REQUEST);
  EXPECT_STREQ(msg.message.request.command, "OPTIONS");
  EXPECT_STREQ(msg.message.request.target, "rtsp://10.0.0.1:48010");
  EXPECT_EQ(msg.sequenceNumber, 1);
  EXPECT_EQ(option(msg, "X-GS-ClientVersion"), "14");
  EXPECT_EQ(msg.payload, nullptr);
  freeMessage(&msg);
}

TEST(RtspParser, RequestWithPayload) {
  RTSP_MESSAGE msg;
  ASSERT_EQ(parse("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq: 6\r\nContent-length: 7\r\n\r\nv=0\r\nx", msg), RTSP_ERROR_SUCCESS);
  EXPECT_EQ(msg.sequenceNumber, 6);
  ASSERT_NE(msg.payload, nullptr);
  EXPECT_EQ(std::string(msg.payload, msg.payloadLength), "v=0\r\nx");
  freeMessage(&msg);
}

TEST(RtspParser, MissingFinalLineBreaksAreAccepted) {
  // Some clients drop the last one or two bytes of the end marker
  RTSP_MESSAGE msg;
  ASSERT_EQ(parse("PLAY / RTSP/1.0\r\nCSeq: 7\r\n", msg), RTSP_ERROR_SUCCESS);
  EXPECT_EQ(msg.sequenceNumber, 7);
  freeMessage(&msg);

  ASSERT_EQ(parse("PLAY / RTSP/1.0\r\nCSeq: 8\r\n\r", msg), RTSP_ERROR_SUCCESS);
  EXPECT_EQ(msg.sequenceNumber, 8);
  freeMessage(&msg);
}

TEST(RtspParser, Response) {
  RTSP_MESSAGE msg;
  ASSERT_EQ(parse("RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n", msg), RTSP_ERROR_SUCCESS);
  EXPECT_EQ(msg.type, TYPE_RESPONSE);
  EXPECT_EQ(msg.message.response.statusCode, 200);
  EXPECT_EQ(msg.sequenceNumber, 3);
  freeMessage(&msg);
}

TEST(RtspParser, MessageEndingInsideAValueIsMalformed) {
  // The last value runs up to the end of the input, so there is no end marker
  RTSP_MESSAGE msg;
  EXPECT_EQ(parse("SETUP streamid=video/0/0 RTSP/1.0\r\nCSeq: 2", msg), RTSP_ERROR_MALFORMED);
  EXPECT_EQ(parse("SETUP streamid=video/0/0 RTSP/1.0\r\nCSeq: 2\r\nTransport: unicast", msg), RTSP_ERROR_MALFORMED);
  EXPECT_EQ(parse("OPTIONS / RTSP/1.0", msg), RTSP_ERROR_MALFORMED);
  // An empty header value makes the bytes after the end marker the value of that header
  EXPECT_EQ(parse("OPTIONS x RTSP/1.0\r\nFoo:\r\n\r\nBar", msg), RTSP_ERROR_MALFORMED);
  EXPECT_EQ(parse("", msg), RTSP_ERROR_MALFORMED);
}
