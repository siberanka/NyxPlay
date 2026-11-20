// Video depacketizer: bounded frame reassembly and rejection of frames without SOF
// (upstream bf71226), plus the normal paths so that the checks cannot pass by rejecting everything.
//
// This file includes VideoDepacketizer.c to reach its static functions.

#include "testutil.h"

#include "VideoDepacketizer.c"

// A non H.264/HEVC codec is passed through as is, which makes every payload byte observable
#define TEST_FORMAT VIDEO_FORMAT_AV1_MAIN8
#define FRAME_HEADER_SIZE 8

static uint32_t streamIndex;
static int declaredLastPayload = -1; // header field with the size of the last packet, -1: this packet

static void setup(void) {
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    memset(&VideoCallbacks, 0, sizeof(VideoCallbacks));
    memset(&ListenerCallbacks, 0, sizeof(ListenerCallbacks));
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = 0;
    NegotiatedVideoFormat = TEST_FORMAT;
    EncryptionFeaturesEnabled = 0;

    // ControlStream.c is linked: LiRequestIdrFrame() and the loss reports need its queues
    CHECK_EQ_INT(initializeControlStream(), 0);
    initializeVideoDepacketizer(1392);
    streamIndex = 0;
}

static void teardown(void) {
    stopVideoDepacketizer();
    destroyVideoDepacketizer();
    destroyControlStream();
}

// Builds and feeds one video packet. 'first' packets get the 8 byte frame header (type 2 = IDR,
// 1 = P frame) in front of 'payloadLen' bytes of picture data.
static void feed(uint32_t frame, uint8_t flags, int frameType, int payloadLen) {
    int headerLen = (flags & FLAG_SOF) ? FRAME_HEADER_SIZE : 0;
    int total = (int)sizeof(NV_VIDEO_PACKET) + headerLen + payloadLen;
    PNV_VIDEO_PACKET pkt = calloc(1, total);
    unsigned char* data = (unsigned char*)(pkt + 1);
    PLENTRY_INTERNAL existing = NULL;

    pkt->streamPacketIndex = (streamIndex++) << 8;
    pkt->frameIndex = frame;
    pkt->flags = flags;
    pkt->multiFecBlocks = 0;
    if (headerLen) {
        data[0] = 0x01;
        data[3] = (unsigned char)frameType;
        // bytes 4 and 5 of the header carry the payload length for non H.264 codecs
        int declared = declaredLastPayload >= 0 ? declaredLastPayload : headerLen + payloadLen;
        data[4] = (unsigned char)(declared & 0xFF);
        data[5] = (unsigned char)((declared >> 8) & 0xFF);
    }
    memset(data + headerLen, 0x42, payloadLen);

    processRtpPayload(pkt, total, 1000000 + frame, 0, 0, &existing);
    free(pkt);
}

static void freeQueued(void) {
    PDECODE_UNIT du;
    VIDEO_FRAME_HANDLE h;
    while (LiPollNextVideoFrame(&h, &du)) {
        LiCompleteVideoFrame(h, 0);
    }
}

static void testInlineLimits(void) {
    CHECK(canAppendVideoFrameFragment(0, 1, 0));
    CHECK(canAppendVideoFrameFragment(MAX_VIDEO_REASSEMBLED_FRAME_SIZE - 10, 10, 5));
    CHECK(!canAppendVideoFrameFragment(MAX_VIDEO_REASSEMBLED_FRAME_SIZE - 10, 11, 5));
    CHECK(!canAppendVideoFrameFragment(MAX_VIDEO_REASSEMBLED_FRAME_SIZE, 1, 0));
    CHECK(!canAppendVideoFrameFragment(0, 0, 0));
    CHECK(!canAppendVideoFrameFragment(0, -1, 0));
    CHECK(!canAppendVideoFrameFragment(-1, 10, 0));
    CHECK(!canAppendVideoFrameFragment(0, MAX_VIDEO_REASSEMBLED_FRAME_SIZE + 1, 0));
    CHECK(canAppendVideoFrameFragment(0, 10, MAX_VIDEO_REASSEMBLED_FRAGMENTS - 1));
    CHECK(!canAppendVideoFrameFragment(0, 10, MAX_VIDEO_REASSEMBLED_FRAGMENTS));

    // A continuation packet needs a frame in progress with the expected index, a start needs none
    CHECK(isValidVideoFramePacket(true, false, 5, 5));
    CHECK(!isValidVideoFramePacket(true, true, 5, 5));
    CHECK(isValidVideoFramePacket(false, true, 5, 5));
    CHECK(!isValidVideoFramePacket(false, false, 5, 5));
    CHECK(!isValidVideoFramePacket(false, true, 6, 5));
}

static void testNormalFrames(void) {
    PDECODE_UNIT du;
    VIDEO_FRAME_HANDLE h;

    setup();

    // IDR frame in one packet
    feed(1, FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA, 2, 100);
    CHECK_EQ_INT(LiGetPendingVideoFrames(), 1);
    CHECK(LiPollNextVideoFrame(&h, &du));
    if (du) {
        CHECK_EQ_INT(du->fullLength, 100);
        CHECK_EQ_INT(du->frameNumber, 1);
        CHECK_EQ_INT(du->frameType, FRAME_TYPE_IDR);
        LiCompleteVideoFrame(h, 0);
    }

    // P frame in three packets
    declaredLastPayload = 500;
    feed(2, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, 1, 300);
    declaredLastPayload = -1;
    feed(2, FLAG_CONTAINS_PIC_DATA, 0, 400);
    feed(2, FLAG_EOF | FLAG_CONTAINS_PIC_DATA, 0, 500);
    CHECK(LiPollNextVideoFrame(&h, &du));
    if (du) {
        CHECK_EQ_INT(du->fullLength, 300 + 400 + 500);
        CHECK_EQ_INT(du->frameNumber, 2);
        LiCompleteVideoFrame(h, 0);
    }
    CHECK_EQ_INT(nalChainDataLength, 0);
    CHECK_EQ_INT(nalChainFragmentCount, 0);
    freeQueued();
    teardown();
}

static void testMissingSofIsRejected(void) {
    setup();

    // A decoder-ready stream first, so that the depacketizer is not waiting for an IDR frame
    feed(1, FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA, 2, 64);
    freeQueued();

    // In-sequence continuation packets without a SOF, across different frame indexes: before the
    // fix these accumulated in the NAL chain without bound
    for (uint32_t frame = 2; frame < 2000; frame++) {
        feed(frame, FLAG_CONTAINS_PIC_DATA, 0, 1000);
        CHECK(!decodingFrame);
        if (nalChainDataLength != 0 || nalChainHead != NULL) {
            CHECK_EQ_INT(nalChainDataLength, 0);
            CHECK(nalChainHead == NULL);
            break;
        }
    }
    CHECK_EQ_INT(LiGetPendingVideoFrames(), 0);

    // The same frame index repeated without SOF
    for (int i = 0; i < 100; i++) {
        feed(5000, FLAG_CONTAINS_PIC_DATA, 0, 1000);
    }
    CHECK_EQ_INT(nalChainDataLength, 0);
    CHECK(nalChainHead == NULL);

    // A second SOF while a frame is still open is invalid, too
    feed(6000, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, 2, 100);
    CHECK(decodingFrame);
    feed(6001, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, 2, 100);
    CHECK(!decodingFrame);
    CHECK_EQ_INT(nalChainDataLength, 0);
    freeQueued();
    teardown();
}

static void testFrameSizeIsBounded(void) {
    int packets = 0;

    setup();
    feed(1, FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA, 2, 64);
    freeQueued();

    // One frame that never ends. Fragments of 1 MiB: the 32 MiB cap must stop it.
    feed(2, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, 1, 1024 * 1024);
    while (decodingFrame && packets < 100) {
        feed(2, FLAG_CONTAINS_PIC_DATA, 0, 1024 * 1024);
        packets++;
        CHECK(nalChainDataLength <= MAX_VIDEO_REASSEMBLED_FRAME_SIZE);
    }
    CHECK(!decodingFrame);
    CHECK(packets <= 33);
    CHECK_EQ_INT(nalChainDataLength, 0); // the chain was freed
    CHECK(nalChainHead == NULL);
    CHECK_EQ_INT(LiGetPendingVideoFrames(), 0);
    freeQueued();
    teardown();
}

static void testFragmentCountIsBounded(void) {
    unsigned int packets = 0;

    setup();
    feed(1, FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA, 2, 64);
    freeQueued();

    // Tiny packets: the byte limit would allow millions, the fragment limit stops it earlier
    feed(2, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, 1, 1);
    while (decodingFrame && packets < MAX_VIDEO_REASSEMBLED_FRAGMENTS + 10) {
        feed(2, FLAG_CONTAINS_PIC_DATA, 0, 1);
        packets++;
        CHECK(nalChainFragmentCount <= MAX_VIDEO_REASSEMBLED_FRAGMENTS);
    }
    CHECK(!decodingFrame);
    CHECK(packets <= MAX_VIDEO_REASSEMBLED_FRAGMENTS);
    CHECK_EQ_INT(nalChainFragmentCount, 0);
    CHECK_EQ_INT(nalChainDataLength, 0);
    freeQueued();
    teardown();
}

static void testEmptyFragmentsDoNotAllocate(void) {
    setup();
    feed(1, FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA, 2, 64);
    freeQueued();

    feed(2, FLAG_SOF | FLAG_CONTAINS_PIC_DATA, 1, 10);
    CHECK_EQ_INT(nalChainFragmentCount, 1);
    for (int i = 0; i < 100000; i++) {
        feed(2, FLAG_CONTAINS_PIC_DATA, 0, 0);
    }
    // Zero byte packets neither create list entries nor end the frame
    CHECK_EQ_INT(nalChainFragmentCount, 1);
    CHECK_EQ_INT(nalChainDataLength, 10);
    freeQueued();
    teardown();
}

int main(void) {
    RUN(testInlineLimits);
    RUN(testNormalFrames);
    RUN(testMissingSofIsRejected);
    RUN(testFrameSizeIsBounded);
    RUN(testFragmentCountIsBounded);
    RUN(testEmptyFragmentsDoNotAllocate);
    return testFinish("video");
}
