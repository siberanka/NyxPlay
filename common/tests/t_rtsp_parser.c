// RTSP message parser (src/RtspParser.c). The vectors are the ones of the host unit test
// host/tests/unit/test_rtsp_parser.cpp, so the library parser is accepted by the host for
// exactly the same inputs. Every input is parsed from a buffer of exactly its length so that
// AddressSanitizer reports a read past the end of the message.

#include "testutil.h"

#include "Rtsp.h"

static int parse(const char* text, size_t len, PRTSP_MESSAGE msg) {
    char* buffer = malloc(len ? len : 1);
    int ret;

    if (len) {
        memcpy(buffer, text, len);
    }
    memset(msg, 0, sizeof(*msg));
    ret = parseRtspMessage(msg, buffer, (int)len);
    free(buffer); // the parser works on its own copy
    return ret;
}

#define PARSE(str, msg) parse((str), sizeof(str) - 1, &(msg))

static const char* option(PRTSP_MESSAGE msg, const char* name) {
    for (POPTION_ITEM opt = msg->options; opt; opt = opt->next) {
        if (opt->option && strcmp(opt->option, name) == 0) {
            return opt->content ? opt->content : "";
        }
    }
    return "";
}

static void testRequestWithEndMarker(void) {
    RTSP_MESSAGE msg;
    CHECK_EQ_INT(PARSE("OPTIONS rtsp://10.0.0.1:48010 RTSP/1.0\r\nCSeq: 1\r\nX-GS-ClientVersion: 14\r\n\r\n", msg), RTSP_ERROR_SUCCESS);
    CHECK_EQ_INT(msg.type, TYPE_REQUEST);
    CHECK(strcmp(msg.message.request.command, "OPTIONS") == 0);
    CHECK(strcmp(msg.message.request.target, "rtsp://10.0.0.1:48010") == 0);
    CHECK_EQ_INT(msg.sequenceNumber, 1);
    CHECK(strcmp(option(&msg, "X-GS-ClientVersion"), "14") == 0);
    CHECK(msg.payload == NULL);
    freeMessage(&msg);
}

static void testRequestWithPayload(void) {
    RTSP_MESSAGE msg;
    CHECK_EQ_INT(PARSE("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq: 6\r\nContent-length: 7\r\n\r\nv=0\r\nx", msg), RTSP_ERROR_SUCCESS);
    CHECK_EQ_INT(msg.sequenceNumber, 6);
    CHECK(msg.payload != NULL);
    CHECK_EQ_INT(msg.payloadLength, 6);
    if (msg.payload != NULL && msg.payloadLength == 6) {
        CHECK(memcmp(msg.payload, "v=0\r\nx", 6) == 0);
    }
    freeMessage(&msg);
}

static void testMissingFinalLineBreaksAreAccepted(void) {
    RTSP_MESSAGE msg;
    CHECK_EQ_INT(PARSE("PLAY / RTSP/1.0\r\nCSeq: 7\r\n", msg), RTSP_ERROR_SUCCESS);
    CHECK_EQ_INT(msg.sequenceNumber, 7);
    freeMessage(&msg);
    CHECK_EQ_INT(PARSE("PLAY / RTSP/1.0\r\nCSeq: 8\r\n\r", msg), RTSP_ERROR_SUCCESS);
    CHECK_EQ_INT(msg.sequenceNumber, 8);
    freeMessage(&msg);
}

static void testResponse(void) {
    RTSP_MESSAGE msg;
    CHECK_EQ_INT(PARSE("RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n", msg), RTSP_ERROR_SUCCESS);
    CHECK_EQ_INT(msg.type, TYPE_RESPONSE);
    CHECK_EQ_INT(msg.message.response.statusCode, 200);
    CHECK_EQ_INT(msg.sequenceNumber, 3);
    freeMessage(&msg);
}

static void testMalformed(void) {
    RTSP_MESSAGE msg;
    CHECK_EQ_INT(PARSE("SETUP streamid=video/0/0 RTSP/1.0\r\nCSeq: 2", msg), RTSP_ERROR_MALFORMED);
    CHECK_EQ_INT(PARSE("SETUP streamid=video/0/0 RTSP/1.0\r\nCSeq: 2\r\nTransport: unicast", msg), RTSP_ERROR_MALFORMED);
    CHECK_EQ_INT(PARSE("OPTIONS / RTSP/1.0", msg), RTSP_ERROR_MALFORMED);
    // The old host parser read the bytes after the end marker as the value of an empty header and
    // rejected the message. The library parser ends the headers at the blank line and keeps the
    // remaining bytes as payload.
    CHECK_EQ_INT(PARSE("OPTIONS x RTSP/1.0\r\nFoo:\r\n\r\nBar", msg), RTSP_ERROR_SUCCESS);
    CHECK(strcmp(option(&msg, "Foo"), "") == 0);
    CHECK(msg.payload != NULL && msg.payloadLength == 3 && memcmp(msg.payload, "Bar", 3) == 0);
    freeMessage(&msg);
    CHECK_EQ_INT(parse("", 0, &msg), RTSP_ERROR_MALFORMED);
}

static void testRepeatedHeaderKeepsTheLastValue(void) {
    RTSP_MESSAGE msg;
    char many[2048];
    size_t n;

    CHECK_EQ_INT(PARSE("OPTIONS x RTSP/1.0\r\nA: 1\r\nCSeq: 4\r\nA: 2\r\n\r\n", msg), RTSP_ERROR_SUCCESS);
    CHECK(strcmp(option(&msg, "A"), "2") == 0);
    CHECK_EQ_INT(msg.sequenceNumber, 4);
    freeMessage(&msg); // every repeated header node must be released here

    n = (size_t)snprintf(many, sizeof(many), "OPTIONS x RTSP/1.0\r\n");
    for (int i = 0; i < 100; i++) {
        n += (size_t)snprintf(many + n, sizeof(many) - n, "A: 1\r\n");
    }
    n += (size_t)snprintf(many + n, sizeof(many) - n, "\r\n");
    CHECK_EQ_INT(parse(many, n, &msg), RTSP_ERROR_SUCCESS);
    CHECK(strcmp(option(&msg, "A"), "1") == 0);
    freeMessage(&msg);
}

// Truncating a valid message at every position and mutating single bytes must never
// read or write outside of the buffer (checked by the sanitizer build).
static void testTruncationAndMutation(void) {
    static const char valid[] = "ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq: 6\r\nContent-length: 7\r\nSession: DEADBEEFCAFE;timeout = 90\r\n\r\nv=0\r\nx";
    RTSP_MESSAGE msg;
    size_t total = sizeof(valid) - 1;

    for (size_t len = 0; len <= total; len++) {
        if (parse(valid, len, &msg) == RTSP_ERROR_SUCCESS) {
            freeMessage(&msg);
        }
    }
    for (size_t i = 0; i < total; i++) {
        char copy[sizeof(valid)];
        memcpy(copy, valid, total);
        copy[i] = (char)(copy[i] ^ 0xFF);
        if (parse(copy, total, &msg) == RTSP_ERROR_SUCCESS) {
            freeMessage(&msg);
        }
        copy[i] = '\0';
        if (parse(copy, total, &msg) == RTSP_ERROR_SUCCESS) {
            freeMessage(&msg);
        }
    }
    CHECK(1);
}

static void testHugeContentLengthIsRejected(void) {
    RTSP_MESSAGE msg;
    int ret = PARSE("ANNOUNCE x RTSP/1.0\r\nCSeq: 1\r\nContent-length: 99999999\r\n\r\nabc", msg);
    // The declared payload is larger than the data that was received: the parser must not
    // trust it (a read of 99999999 bytes would be reported by the sanitizer).
    if (ret == RTSP_ERROR_SUCCESS) {
        CHECK(msg.payloadLength <= 3);
        freeMessage(&msg);
    }
    else {
        CHECK_EQ_INT(ret, RTSP_ERROR_MALFORMED);
    }
}

int main(void) {
    RUN(testRequestWithEndMarker);
    RUN(testRequestWithPayload);
    RUN(testMissingFinalLineBreaksAreAccepted);
    RUN(testResponse);
    RUN(testMalformed);
    RUN(testRepeatedHeaderKeepsTheLastValue);
    RUN(testTruncationAndMutation);
    RUN(testHugeContentLengthIsRejected);
    return testFinish("rtsp_parser");
}
