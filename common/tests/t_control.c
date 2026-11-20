// Control stream: packet type tables, the NyxPlay text input extension, the ENet disconnect
// intercept (upstream e80a786, 41cedb8) and the encrypted message formats shared with the host.
//
// This file includes ControlStream.c to reach its static functions.

#include "testutil.h"

#include "ControlStream.c"

// ---------------------------------------------------------------- tables
static const short expectedGen7Enc[IDX_COUNT] = {
    0x0302, 0x0307, 0x0301, 0x0201, 0x0204, 0x0206, 0x010b, 0x0109, 0x010e,
    0x5500, 0x5501, 0x5502, 0x3000, 0x3001, 0x3002, 0x5503, 0x4E01,
};

static void testTableLayout(void) {
    const short* tables[] = { packetTypesGen3, packetTypesGen4, packetTypesGen5, packetTypesGen7, packetTypesGen7Enc };

    CHECK_EQ_INT(IDX_COUNT, 17);
    CHECK_EQ_INT(IDX_EXEC_SERVER_CMD, 12);
    CHECK_EQ_INT(IDX_SET_CLIPBOARD, 13);
    CHECK_EQ_INT(IDX_FILE_TRANSFER_NONCE_REQUEST, 14);
    CHECK_EQ_INT(IDX_DS_ADAPTIVE_TRIGGERS, 15);
    CHECK_EQ_INT(IDX_SET_TEXT_INPUT_STATE, 16);

    // The encrypted table is the only one carrying the extensions, in index order
    for (int i = 0; i < IDX_COUNT; i++) {
        CHECK_EQ_INT(packetTypesGen7Enc[i], expectedGen7Enc[i]);
    }
    for (int t = 0; t < 4; t++) {
        for (int i = IDX_RUMBLE_TRIGGER_DATA; i < IDX_COUNT; i++) {
            CHECK_EQ_INT(tables[t][i], -1);
        }
    }

    // Values within one table are unique (a duplicate would make the receive switch ambiguous)
    for (int t = 0; t < 5; t++) {
        for (int i = 0; i < IDX_COUNT; i++) {
            for (int j = i + 1; j < IDX_COUNT; j++) {
                if (tables[t][i] != -1) {
                    CHECK(tables[t][i] != tables[t][j]);
                }
            }
        }
    }

    // The shared constants of NyxExtensions.h are the values on the wire
    CHECK_EQ_INT(LI_PACKET_TYPE_EXEC_SERVER_CMD, 0x3000);
    CHECK_EQ_INT(LI_PACKET_TYPE_SET_CLIPBOARD, 0x3001);
    CHECK_EQ_INT(LI_PACKET_TYPE_FILE_TRANSFER_NONCE_REQUEST, 0x3002);
    CHECK_EQ_INT(LI_PACKET_TYPE_SET_ADAPTIVE_TRIGGERS, 0x5503);
    CHECK_EQ_INT(LI_PACKET_TYPE_SET_TEXT_INPUT_STATE, 0x4E01);
    CHECK_EQ_INT(LI_TEXT_INPUT_PAYLOAD_SIZE, 4);
    CHECK_EQ_INT(CTRL_CHANNEL_SERVERCTL, 0x08);
}

// ---------------------------------------------------------------- async callbacks
static PQUEUED_ASYNC_CALLBACK pollAsync(void) {
    PQUEUED_ASYNC_CALLBACK cb = NULL;
    if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&cb) != LBQ_SUCCESS) {
        return NULL;
    }
    return cb;
}

static void queueRaw(unsigned short type, const unsigned char* payload, int payloadLen) {
    unsigned char* buf = malloc(sizeof(NVCTL_ENET_PACKET_HEADER_V1) + payloadLen);
    NVCTL_ENET_PACKET_HEADER_V1 hdr;

    hdr.type = type;
    memcpy(buf, &hdr, sizeof(hdr));
    if (payloadLen) {
        memcpy(buf + sizeof(hdr), payload, payloadLen);
    }
    CHECK(needsAsyncCallback(type));
    queueAsyncCallback((PNVCTL_ENET_PACKET_HEADER_V1)buf, (int)sizeof(hdr) + payloadLen);
    free(buf);
}

static void testTextInputDecoding(void) {
    PQUEUED_ASYNC_CALLBACK cb;

    LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 30);
    packetTypes = (short*)packetTypesGen7Enc;

    {
        const unsigned char focused[] = { 1, 1, LI_TEXT_INPUT_FLAG_PASSWORD | LI_TEXT_INPUT_FLAG_MULTILINE, 0 };
        queueRaw(0x4E01, focused, sizeof(focused));
        cb = pollAsync();
        CHECK(cb != NULL);
        if (cb) {
            CHECK_EQ_INT(cb->typeIndex, IDX_SET_TEXT_INPUT_STATE);
            CHECK_EQ_INT(cb->data.textInputState.active, 1);
            CHECK_EQ_INT(cb->data.textInputState.flags, 3);
            free(cb);
        }
    }
    {
        // Version 0 is malformed: reported as "no text field"
        const unsigned char bad[] = { 0, 1, 3, 0 };
        queueRaw(0x4E01, bad, sizeof(bad));
        cb = pollAsync();
        CHECK(cb != NULL);
        if (cb) {
            CHECK_EQ_INT(cb->data.textInputState.active, 0);
            CHECK_EQ_INT(cb->data.textInputState.flags, 0);
            free(cb);
        }
    }
    {
        // A truncated payload must not read beyond the packet (the heap buffer is exactly sized)
        const unsigned char shortPayload[] = { 1 };
        queueRaw(0x4E01, shortPayload, sizeof(shortPayload));
        cb = pollAsync();
        CHECK(cb != NULL);
        if (cb) {
            CHECK_EQ_INT(cb->data.textInputState.active, 0);
            free(cb);
        }
        queueRaw(0x4E01, NULL, 0);
        cb = pollAsync();
        CHECK(cb != NULL);
        if (cb) {
            CHECK_EQ_INT(cb->data.textInputState.active, 0);
            free(cb);
        }
    }
    {
        // Apollo clipboard and nonce messages have no client handler: they are dropped, not queued
        const unsigned char data[] = { 1, 2, 3, 4 };
        queueRaw(0x3001, data, sizeof(data));
        CHECK(pollAsync() == NULL);
        queueRaw(0x3002, data, sizeof(data));
        CHECK(pollAsync() == NULL);
    }

    // The legacy tables never match an extension type
    packetTypes = (short*)packetTypesGen7;
    CHECK(!needsAsyncCallback(0x4E01));
    CHECK(!needsAsyncCallback(0x3000));
    packetTypes = (short*)packetTypesGen7Enc;
    CHECK(needsAsyncCallback(0x5503));
    CHECK(!needsAsyncCallback(0x3000)); // exec server command is client to host only
    CHECK(!needsAsyncCallback(0x0206));

    LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue);
}

// ---------------------------------------------------------------- ENet disconnect intercept
static ENetHost* testHost;
static ENetPeer testPeer;

static size_t makeDatagram(uint8_t* buf, uint16_t peerId, uint8_t session, bool sentTime, bool checksum, uint8_t command) {
    ENetProtocolHeader hdr;
    ENetProtocolDisconnect disc;
    uint16_t wirePeer = (uint16_t)(peerId | ((uint16_t)session << ENET_PROTOCOL_HEADER_SESSION_SHIFT));
    size_t n;

    if (sentTime) {
        wirePeer |= ENET_PROTOCOL_HEADER_FLAG_SENT_TIME;
    }
    hdr.peerID = ENET_HOST_TO_NET_16(wirePeer);
    hdr.sentTime = ENET_HOST_TO_NET_16(0x1234);
    n = sentTime ? sizeof(hdr) : sizeof(hdr) - sizeof(hdr.sentTime);
    memcpy(buf, &hdr, n);
    if (checksum) {
        memset(buf + n, 0xCC, 4);
        n += 4;
    }
    memset(&disc, 0, sizeof(disc));
    disc.header.command = command;
    disc.header.channelID = 0xFF;
    memcpy(buf + n, &disc, sizeof(disc));
    return n + sizeof(disc);
}

static void setAddr(ENetAddress* a, unsigned int hostOrderIp, unsigned short port) {
    struct sockaddr_in* sin = (struct sockaddr_in*)&a->address;
    memset(a, 0, sizeof(*a));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(hostOrderIp);
    sin->sin_port = htons(port);
    a->addressLength = sizeof(*sin);
}

static unsigned int fakeChecksum(const ENetBuffer* buffers, size_t bufferCount) {
    (void)buffers;
    (void)bufferCount;
    return 0;
}

// Feeds a datagram of exactly 'len' bytes (heap allocated so the sanitizer sees any over-read)
static int runIntercept(const uint8_t* datagram, size_t len, bool useChecksum) {
    uint8_t* exact = malloc(len ? len : 1);
    ENetEvent event;
    int ret;

    memcpy(exact, datagram, len);
    testHost->receivedData = exact;
    testHost->receivedDataLength = len;
    testHost->checksum = useChecksum ? fakeChecksum : NULL;
    memset(&event, 0, sizeof(event));
    event.type = ENET_EVENT_TYPE_CONNECT;
    disconnectPending = false;
    ret = ignoreDisconnectIntercept(testHost, &event);
    testHost->receivedData = NULL;
    free(exact);
    return ret;
}

static void testDisconnectIntercept(void) {
    uint8_t buf[64];
    size_t len;

    testHost = calloc(1, sizeof(*testHost));
    memset(&testPeer, 0, sizeof(testPeer));
    testPeer.incomingPeerID = 5;
    testPeer.incomingSessionID = 2;
    setAddr(&testPeer.address, 0xC000020Au, 47999);
    testHost->receivedPeerAddress = testPeer.address;
    peer = &testPeer;

    // A disconnect from our peer is intercepted in all four header layouts
    len = makeDatagram(buf, 5, 2, false, false, ENET_PROTOCOL_COMMAND_DISCONNECT);
    CHECK_EQ_INT(runIntercept(buf, len, false), 1);
    CHECK(disconnectPending);
    len = makeDatagram(buf, 5, 2, true, false, ENET_PROTOCOL_COMMAND_DISCONNECT);
    CHECK_EQ_INT(runIntercept(buf, len, false), 1);
    CHECK(disconnectPending);
    len = makeDatagram(buf, 5, 2, true, true, ENET_PROTOCOL_COMMAND_DISCONNECT);
    CHECK_EQ_INT(runIntercept(buf, len, true), 1);
    CHECK(disconnectPending);
    len = makeDatagram(buf, 5, 2, false, true, ENET_PROTOCOL_COMMAND_DISCONNECT);
    CHECK_EQ_INT(runIntercept(buf, len, true), 1);
    CHECK(disconnectPending);

    // Disconnects for other peers, sessions or from another address are left to ENet
    len = makeDatagram(buf, 6, 2, false, false, ENET_PROTOCOL_COMMAND_DISCONNECT);
    CHECK_EQ_INT(runIntercept(buf, len, false), 0);
    CHECK(!disconnectPending);
    len = makeDatagram(buf, 5, 1, false, false, ENET_PROTOCOL_COMMAND_DISCONNECT);
    CHECK_EQ_INT(runIntercept(buf, len, false), 0);
    CHECK(!disconnectPending);
    len = makeDatagram(buf, 5, 2, false, false, ENET_PROTOCOL_COMMAND_DISCONNECT);
    setAddr(&testHost->receivedPeerAddress, 0xC0000263u, 47999);
    CHECK_EQ_INT(runIntercept(buf, len, false), 0);
    CHECK(!disconnectPending);
    testHost->receivedPeerAddress = testPeer.address;
    peer = NULL;
    CHECK_EQ_INT(runIntercept(buf, len, false), 0);
    CHECK(!disconnectPending);
    peer = &testPeer;

    // Not a disconnect command
    len = makeDatagram(buf, 5, 2, false, false, ENET_PROTOCOL_COMMAND_PING);
    CHECK_EQ_INT(runIntercept(buf, len, false), 0);
    CHECK(!disconnectPending);

    // Wrong sizes: nothing may be read beyond the datagram. This includes every length that
    // is too short to even contain a command header, and trailing bytes after the command.
    len = makeDatagram(buf, 5, 2, false, false, ENET_PROTOCOL_COMMAND_DISCONNECT);
    for (size_t cut = 0; cut < len; cut++) {
        CHECK_EQ_INT(runIntercept(buf, cut, false), 0);
        CHECK(!disconnectPending);
    }
    memset(buf + len, 0, 8);
    CHECK_EQ_INT(runIntercept(buf, len + 1, false), 0);
    CHECK(!disconnectPending);

    // The sentTime flag claims a longer header than the datagram holds
    {
        uint16_t flagged = ENET_HOST_TO_NET_16((uint16_t)(5 | (2 << ENET_PROTOCOL_HEADER_SESSION_SHIFT) | ENET_PROTOCOL_HEADER_FLAG_SENT_TIME));
        memcpy(buf, &flagged, 2);
    }
    CHECK_EQ_INT(runIntercept(buf, len, false), 0);
    CHECK(!disconnectPending);

    peer = NULL;
    free(testHost);
}

// ---------------------------------------------------------------- encrypted control messages
static const unsigned char vectorKey[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

// Client to host: input data (type 0x0206) with an 8 byte payload, sequence 5, control v2
static const char clientToHostHex[] = "01002000050000009139b8d9f73482e1312dc4aa17844b3ca7fe544c76f9157c1ce78319";
// Host to client: NyxPlay text input state (0x4E01: version 1, active, password and multiline), sequence 9
static const char hostToClientHex[] = "01001c0009000000cbc66978f1d6e8c86fb230b21ce46cafc6eafffb12646ff3";

static void printHex(const char* label, const uint8_t* d, size_t n) {
    printf("VECTOR %s ", label);
    for (size_t i = 0; i < n; i++) {
        printf("%02x", d[i]);
    }
    printf("\n");
}

static void testEncryptedFormats(void) {
    uint8_t packet[128];
    uint8_t expected[128];
    size_t expectedLen;
    PNVCTL_ENCRYPTED_PACKET_HEADER enc = (PNVCTL_ENCRYPTED_PACKET_HEADER)packet;
    int total;

    memcpy(StreamConfig.remoteInputAesKey, vectorKey, sizeof(vectorKey));
    EncryptionFeaturesEnabled = SS_ENC_CONTROL_V2;
    encryptionCtx = PltCreateCryptoContext(0);
    decryptionCtx = PltCreateCryptoContext(CTRL_CHANNEL_COUNT);

    // --- client to host, sealed by the library's own function
    {
        uint8_t plain[4 + 8] = { 0x06, 0x02, 0x08, 0x00, 1, 2, 3, 4, 5, 6, 7, 8 };
        memset(packet, 0, sizeof(packet));
        enc->encryptedHeaderType = 0x0001;
        enc->length = (unsigned short)(sizeof(enc->seq) + AES_GCM_TAG_LENGTH + sizeof(plain));
        enc->seq = 5;
        // encryptControlMessage() reads the V2 header from 'plain' and writes tag and ciphertext after enc
        CHECK(encryptControlMessage(enc, (PNVCTL_ENET_PACKET_HEADER_V2)plain));
        total = (int)(sizeof(*enc) + AES_GCM_TAG_LENGTH + sizeof(plain));
        printHex("clientToHost", packet, total);
        hexToBytes(clientToHostHex, expected, &expectedLen);
        CHECK_EQ_INT(expectedLen, (size_t)total);
        CHECK(expectedLen == (size_t)total && memcmp(packet, expected, total) == 0);
    }

    // --- host to client: sealed with the host's construction, opened by the library
    {
        uint8_t hostPacket[128];
        uint8_t iv[12] = { 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'H', 'C' };
        uint8_t plain[4 + 4] = { 0x01, 0x4E, 0x04, 0x00, 1, 1, 3, 0 };
        PPLT_CRYPTO_CONTEXT hostCtx = PltCreateCryptoContext(0);
        PNVCTL_ENCRYPTED_PACKET_HEADER hostEnc = (PNVCTL_ENCRYPTED_PACKET_HEADER)hostPacket;
        int outLen = 0;
        PNVCTL_ENET_PACKET_HEADER_V1 v1 = NULL;
        int v1Len = 0;

        hostEnc->encryptedHeaderType = 0x0001;
        hostEnc->length = (unsigned short)(sizeof(hostEnc->seq) + AES_GCM_TAG_LENGTH + sizeof(plain));
        hostEnc->seq = 9;
        CHECK(PltEncryptMessage(hostCtx, ALGORITHM_AES_GCM, 0, (unsigned char*)vectorKey, 16, iv, 12,
                                hostPacket + sizeof(*hostEnc), AES_GCM_TAG_LENGTH, plain, sizeof(plain),
                                hostPacket + sizeof(*hostEnc) + AES_GCM_TAG_LENGTH, &outLen));
        total = (int)(sizeof(*hostEnc) + AES_GCM_TAG_LENGTH + outLen);
        printHex("hostToClient", hostPacket, total);
        hexToBytes(hostToClientHex, expected, &expectedLen);
        CHECK_EQ_INT(expectedLen, (size_t)total);
        CHECK(expectedLen == (size_t)total && memcmp(hostPacket, expected, total) == 0);

        // The library decrypts the host vector and converts it to the V1 layout
        if (expectedLen == (size_t)total) {
            memcpy(hostPacket, expected, total);
            CHECK(decryptControlMessageToV1(0, (PNVCTL_ENCRYPTED_PACKET_HEADER)hostPacket, total, &v1, &v1Len));
            if (v1 != NULL) {
                CHECK_EQ_INT(v1Len, 2 + 4);
                CHECK_EQ_INT(v1->type, 0x4E01);
                CHECK(memcmp((uint8_t*)v1 + 2, "\x01\x01\x03\x00", 4) == 0);
                free(v1);
            }
            // Replaying the same packet on the same channel is rejected, another channel is independent
            v1 = NULL;
            CHECK(!decryptControlMessageToV1(0, (PNVCTL_ENCRYPTED_PACKET_HEADER)hostPacket, total, &v1, &v1Len));
            CHECK(v1 == NULL);
            CHECK(decryptControlMessageToV1(1, (PNVCTL_ENCRYPTED_PACKET_HEADER)hostPacket, total, &v1, &v1Len));
            free(v1);
        }
        PltDestroyCryptoContext(hostCtx);
    }

    // A flipped ciphertext bit fails the tag check
    {
        uint8_t copy[128];
        PNVCTL_ENET_PACKET_HEADER_V1 v1 = NULL;
        int v1Len = 0;

        hexToBytes(hostToClientHex, copy, &expectedLen);
        copy[sizeof(NVCTL_ENCRYPTED_PACKET_HEADER) + AES_GCM_TAG_LENGTH] ^= 1;
        CHECK(!decryptControlMessageToV1(7, (PNVCTL_ENCRYPTED_PACKET_HEADER)copy, (int)expectedLen, &v1, &v1Len));
    }

    PltDestroyCryptoContext(encryptionCtx);
    PltDestroyCryptoContext(decryptionCtx);
    encryptionCtx = decryptionCtx = NULL;
}

int main(void) {
    RUN(testTableLayout);
    RUN(testTextInputDecoding);
    RUN(testDisconnectIntercept);
    RUN(testEncryptedFormats);
    return testFinish("control");
}
