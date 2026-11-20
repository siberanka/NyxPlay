// Replay detection of authenticated control stream and RTSP messages (upstream c7dff22)
// and the matching host behaviour: one strictly increasing sequence per ENet channel.

#include "testutil.h"

#include "Limelight-internal.h"
#include "PlatformCrypto.h"

static const unsigned char key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static PPLT_CRYPTO_CONTEXT sealCtx;

// Seals "payload" the way the host does for control v2: 12 byte IV, little endian sequence
// number in the first four bytes, 'H' 'C' in the last two.
static void seal(unsigned int seq, unsigned char* out, unsigned char* tag, int* outLen) {
    unsigned char iv[12] = { 0 };
    unsigned char plain[24];

    memset(plain, 0x5a, sizeof(plain));
    iv[0] = (unsigned char)seq;
    iv[1] = (unsigned char)(seq >> 8);
    iv[2] = (unsigned char)(seq >> 16);
    iv[3] = (unsigned char)(seq >> 24);
    iv[10] = 'H';
    iv[11] = 'C';
    CHECK(PltEncryptMessage(sealCtx, ALGORITHM_AES_GCM, CIPHER_FLAG_RESET_IV, (unsigned char*)key, sizeof(key),
                            iv, sizeof(iv), tag, 16, plain, sizeof(plain), out, outLen));
}

// Returns true if the message was accepted
static bool receive(PPLT_CRYPTO_CONTEXT ctx, unsigned char channel, unsigned int seq, int flags, bool corruptTag) {
    unsigned char iv[12] = { 0 };
    unsigned char sealed[64], tag[16], plain[64];
    int sealedLen = 0, plainLen = 0;

    seal(seq, sealed, tag, &sealedLen);
    if (corruptTag) {
        tag[3] ^= 0x40;
    }

    iv[0] = (unsigned char)seq;
    iv[1] = (unsigned char)(seq >> 8);
    iv[2] = (unsigned char)(seq >> 16);
    iv[3] = (unsigned char)(seq >> 24);
    iv[10] = 'H';
    iv[11] = 'C';
    return PltDecryptMessage(ctx, ALGORITHM_AES_GCM, flags, channel, seq, (unsigned char*)key, sizeof(key),
                             iv, sizeof(iv), tag, 16, sealed, sealedLen, plain, &plainLen);
}

static void testInOrderStream(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    // The host sends one strictly increasing sequence on channel 0, with and without gaps
    for (unsigned int seq = 0; seq <= 700; seq++) {
        CHECK(receive(ctx, 0, seq, 0, false));
    }
    for (unsigned int seq = 701; seq < 5000; seq += 3) {
        CHECK(receive(ctx, 0, seq, 0, false));
    }
    PltDestroyCryptoContext(ctx);
}

static void testDuplicatesAreRejected(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    for (unsigned int seq = 0; seq <= 600; seq++) {
        CHECK(receive(ctx, 0, seq, 0, false));
    }
    CHECK(!receive(ctx, 0, 600, 0, false)); // the newest one again
    CHECK(!receive(ctx, 0, 599, 0, false));
    CHECK(!receive(ctx, 0, 300, 0, false));
    CHECK(!receive(ctx, 0, 89, 0, false));  // exactly 511 behind, already seen
    CHECK(!receive(ctx, 0, 88, 0, false));  // 512 behind: outside of the window
    CHECK(!receive(ctx, 0, 0, 0, false));
    PltDestroyCryptoContext(ctx);
}

static void testOutOfOrderWithinWindow(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    CHECK(receive(ctx, 0, 1000, 0, false));
    CHECK(receive(ctx, 0, 1010, 0, false));
    CHECK(receive(ctx, 0, 1005, 0, false));  // late but unseen
    CHECK(!receive(ctx, 0, 1005, 0, false)); // now it is a replay
    CHECK(receive(ctx, 0, 1010 - 511, 0, false)); // oldest slot of the window
    CHECK(!receive(ctx, 0, 1010 - 511, 0, false));
    CHECK(!receive(ctx, 0, 1010 - 512, 0, false)); // too old
    CHECK(receive(ctx, 0, 1000 + 600, 0, false));  // jump ahead shifts the window
    CHECK(!receive(ctx, 0, 1010, 0, false));       // 590 behind the new maximum
    CHECK(!receive(ctx, 0, 1600, 0, false));
    // A jump of more than the window size clears all history
    CHECK(receive(ctx, 0, 1600 + 100000, 0, false));
    CHECK(!receive(ctx, 0, 1600, 0, false));
    PltDestroyCryptoContext(ctx);
}

static void testSequenceWraparound(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);
    unsigned int seq;

    for (seq = 0xFFFFFFF0u; seq != 0xFFFFFFFFu; seq++) {
        CHECK(receive(ctx, 1, seq, 0, false));
    }
    CHECK(receive(ctx, 1, 0xFFFFFFFFu, 0, false));
    for (seq = 0; seq < 16; seq++) {
        CHECK(receive(ctx, 1, seq, 0, false)); // newer than 0xFFFFFFFF in modular arithmetic
    }
    CHECK(!receive(ctx, 1, 0xFFFFFFFFu, 0, false));
    CHECK(!receive(ctx, 1, 0xFFFFFFF0u, 0, false));
    CHECK(!receive(ctx, 1, 15, 0, false));
    PltDestroyCryptoContext(ctx);
}

static void testChannelsAreIndependent(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    CHECK(receive(ctx, 0, 50, 0, false));
    CHECK(receive(ctx, 3, 50, 0, false));
    CHECK(receive(ctx, 0x2F, 50, 0, false)); // last valid channel
    CHECK(!receive(ctx, 0, 50, 0, false));
    CHECK(!receive(ctx, 3, 50, 0, false));
    CHECK(receive(ctx, 3, 51, 0, false));
    CHECK(!receive(ctx, CTRL_CHANNEL_COUNT, 1, 0, false)); // out of range index is refused
    CHECK(!receive(ctx, 0xFF, 1, 0, false));
    PltDestroyCryptoContext(ctx);
}

static void testNoReplayCheckFlag(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    // Legacy (non control v2) streams repeat their truncated sequence numbers
    CHECK(receive(ctx, 0, 7, CIPHER_FLAG_NO_REPLAY_CHECK, false));
    CHECK(receive(ctx, 0, 7, CIPHER_FLAG_NO_REPLAY_CHECK, false));
    CHECK(receive(ctx, 0, 3, CIPHER_FLAG_NO_REPLAY_CHECK, false));
    // and they must not poison the window of checked messages
    CHECK(receive(ctx, 0, 7, 0, false));
    CHECK(!receive(ctx, 0, 7, 0, false));
    PltDestroyCryptoContext(ctx);
}

static void testForgedMessagesDoNotAdvanceTheWindow(void) {
    PPLT_CRYPTO_CONTEXT ctx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    CHECK(receive(ctx, 0, 10, 0, false));
    CHECK(!receive(ctx, 0, 0x7FFFFF00u, 0, true)); // bad tag with a far future sequence number
    CHECK(!receive(ctx, 0, 11, 0, true));
    CHECK(receive(ctx, 0, 11, 0, false)); // the real message still goes through
    CHECK(receive(ctx, 0, 12, 0, false));
    PltDestroyCryptoContext(ctx);
}

int main(void) {
    sealCtx = PltCreateCryptoContext(0);
    CHECK(sealCtx != NULL);
    RUN(testInOrderStream);
    RUN(testDuplicatesAreRejected);
    RUN(testOutOfOrderWithinWindow);
    RUN(testSequenceWraparound);
    RUN(testChannelsAreIndependent);
    RUN(testNoReplayCheckFlag);
    RUN(testForgedMessagesDoNotAdvanceTheWindow);
    PltDestroyCryptoContext(sealCtx);
    return testFinish("replay");
}
