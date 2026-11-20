// Audio packet handling: packets without payload are rejected (upstream 18922a1), encrypted
// audio is decrypted into a buffer that is large enough for the padding block (2b45ee9).
//
// This file includes AudioStream.c to reach its static functions.

#include "testutil.h"

#include "AudioStream.c"

#include <openssl/evp.h>

static int callbackCount;
static int lastLength;
static int lastWasNull;
static unsigned char lastData[2048];

static void recordingDecode(char* sampleData, int sampleLength) {
    callbackCount++;
    lastLength = sampleLength;
    lastWasNull = sampleData == NULL;
    if (sampleData != NULL && sampleLength > 0 && sampleLength <= (int)sizeof(lastData)) {
        memcpy(lastData, sampleData, sampleLength);
    }
}

static PQUEUED_AUDIO_PACKET newPacket(unsigned short seq, const unsigned char* payload, int payloadLen) {
    PQUEUED_AUDIO_PACKET p = calloc(1, sizeof(*p));
    PRTP_PACKET rtp = (PRTP_PACKET)p->data;

    rtp->header = 0x80;
    rtp->packetType = 97;
    rtp->sequenceNumber = seq;
    if (payloadLen > 0) {
        memcpy(p->data + sizeof(*rtp), payload, payloadLen);
    }
    p->header.size = (int)sizeof(*rtp) + payloadLen;
    return p;
}

static void setup(bool encrypted) {
    memset(&AudioCallbacks, 0, sizeof(AudioCallbacks));
    AudioCallbacks.decodeAndPlaySample = recordingDecode;
    AudioEncryptionEnabled = encrypted;
    callbackCount = 0;
    lastLength = -1;
    lastWasNull = 0;
    lastSeq = 0;
    for (int i = 0; i < 16; i++) {
        StreamConfig.remoteInputAesKey[i] = (char)(0x30 + i);
    }
    avRiKeyId = 0x01020304;
    audioDecryptionCtx = PltCreateCryptoContext(0);
}

static void teardown(void) {
    PltDestroyCryptoContext(audioDecryptionCtx);
    audioDecryptionCtx = NULL;
}

// AES-128-CBC with PKCS7 padding, IV as built by the receiver
static int sealAudio(unsigned short seq, const unsigned char* plain, int plainLen, unsigned char* out) {
    unsigned char iv[16] = { 0 };
    uint32_t ivSeq = BE32(avRiKeyId + seq);
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int n = 0, tail = 0;

    memcpy(iv, &ivSeq, sizeof(ivSeq));
    EVP_EncryptInit_ex(c, EVP_aes_128_cbc(), NULL, (unsigned char*)StreamConfig.remoteInputAesKey, iv);
    EVP_EncryptUpdate(c, out, &n, plain, plainLen);
    EVP_EncryptFinal_ex(c, out + n, &tail);
    EVP_CIPHER_CTX_free(c);
    return n + tail;
}

static void testPlainPackets(void) {
    unsigned char opus[40];
    PQUEUED_AUDIO_PACKET p;

    setup(false);
    memset(opus, 0x7e, sizeof(opus));

    p = newPacket(1, opus, sizeof(opus));
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 1);
    CHECK_EQ_INT(lastLength, (int)sizeof(opus));
    free(p);

    // A packet that consists of the RTP header only has no audio: nothing may be decoded
    p = newPacket(2, NULL, 0);
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 1);
    free(p);

    // size 0 marks a missing packet and triggers the concealment of the decoder
    p = newPacket(3, NULL, 0);
    p->header.size = 0;
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 2);
    CHECK(lastWasNull);
    CHECK_EQ_INT(lastLength, 0);
    free(p);
    teardown();
}

static void testEncryptedPackets(void) {
    unsigned char plain[1360], sealed[1360 + 16];
    int sealedLen;
    PQUEUED_AUDIO_PACKET p;

    setup(true);
    for (int i = 0; i < (int)sizeof(plain); i++) {
        plain[i] = (unsigned char)(i * 13 + 1);
    }

    // Plaintext sizes that are a multiple of the AES block size get a full padding block, so the
    // ciphertext is 16 bytes longer. The decrypt buffer on the stack must hold all of it.
    sealedLen = sealAudio(10, plain, sizeof(plain), sealed);
    CHECK_EQ_INT(sealedLen, (int)sizeof(plain) + 16);
    p = newPacket(10, sealed, sealedLen);
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 1);
    CHECK_EQ_INT(lastLength, (int)sizeof(plain));
    CHECK(lastLength == (int)sizeof(plain) && memcmp(lastData, plain, sizeof(plain)) == 0);
    free(p);

    // A short, typical Opus packet
    sealedLen = sealAudio(11, plain, 77, sealed);
    p = newPacket(11, sealed, sealedLen);
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 2);
    CHECK_EQ_INT(lastLength, 77);
    free(p);

    // An empty plaintext encrypts to one padding block and decrypts to nothing: rejected
    sealedLen = sealAudio(12, plain, 0, sealed);
    CHECK_EQ_INT(sealedLen, 16);
    p = newPacket(12, sealed, sealedLen);
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 2);
    free(p);

    // No payload at all (nothing to decrypt)
    p = newPacket(13, NULL, 0);
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 2);
    free(p);

    // Garbage that is not a valid padded block (wrong key stream) is dropped
    memset(sealed, 0x41, 32);
    p = newPacket(14, sealed, 32);
    decodeInputData(p);
    CHECK_EQ_INT(callbackCount, 2);
    free(p);
    teardown();
}

int main(void) {
    RUN(testPlainPackets);
    RUN(testEncryptedPackets);
    return testFinish("audio");
}
