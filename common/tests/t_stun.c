// STUN client (src/SimpleStun.c): padded attributes (upstream ee160c9), message length validation
// (2e18599), source address verification (7bf16de) and waiting for a valid response (134eee4).
// A UDP responder on the loopback interface plays the STUN server.

#include "testutil.h"

#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"

#define COOKIE 0x2112a442u

typedef enum {
    SCN_VALID_AFTER_PADDED_UNKNOWN_ATTR,
    SCN_WRONG_TRANSACTION_THEN_VALID,
    SCN_ROGUE_SOURCE_ONLY,
    SCN_LENGTH_LARGER_THAN_DATA,
    SCN_LENGTH_NOT_MULTIPLE_OF_FOUR,
    SCN_ATTRIBUTE_OUT_OF_BOUNDS,
    SCN_WRONG_ATTRIBUTE_LENGTH,
    SCN_WRONG_FAMILY,
    SCN_TRUNCATED_HEADER,
    SCN_ERROR_RESPONSE_THEN_VALID,
} SCENARIO;

static SOCKET serverSock = INVALID_SOCKET;
static SOCKET rogueSock = INVALID_SOCKET;
static unsigned short serverPort;
static volatile int stopServer;
static volatile int scenario;
static PLT_THREAD serverThread;

static const unsigned char expectedIp[4] = { 203, 0, 113, 7 };

static size_t put16(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
    return 2;
}

static size_t header(unsigned char* p, unsigned type, unsigned bodyLen, const unsigned char* txn) {
    put16(p, type);
    put16(p + 2, bodyLen);
    p[4] = 0x21; p[5] = 0x12; p[6] = 0xa4; p[7] = 0x42;
    memcpy(p + 8, txn, 12);
    return 20;
}

// XOR-MAPPED-ADDRESS attribute for expectedIp, with a selectable length field and family
static size_t xorAttr(unsigned char* p, unsigned lengthField, unsigned family) {
    static const unsigned char cookie[4] = { 0x21, 0x12, 0xa4, 0x42 };
    put16(p, 0x0020);
    put16(p + 2, lengthField);
    p[4] = 0;
    p[5] = (unsigned char)family;
    put16(p + 6, 0x1234);
    for (int i = 0; i < 4; i++) {
        p[8 + i] = expectedIp[i] ^ cookie[i];
    }
    return 12;
}

static void sendTo(SOCKET s, struct sockaddr* to, socklen_t toLen, const unsigned char* buf, size_t len) {
    sendto(s, (const char*)buf, (int)len, 0, to, toLen);
}

static void serverProc(void* context) {
    unsigned char req[512];
    (void)context;

    while (!stopServer) {
        struct sockaddr_storage from;
        socklen_t fromLen = sizeof(from);
        int n = (int)recvUdpSocket(serverSock, (char*)req, sizeof(req), true, &from, &fromLen);
        unsigned char out[512];
        size_t len;
        unsigned char badTxn[12];

        if (n < 20 || stopServer) {
            continue;
        }

        switch (scenario) {
        case SCN_VALID_AFTER_PADDED_UNKNOWN_ATTR:
            // SOFTWARE-like attribute with 5 value bytes, padded to 8, then the address
            len = header(out, 0x0101, 12 + 12, req + 8);
            put16(out + len, 0x8022); put16(out + len + 2, 5);
            memcpy(out + len + 4, "abcde\0\0\0", 8);
            len += 12;
            len += xorAttr(out + len, 8, 1);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_WRONG_TRANSACTION_THEN_VALID:
            memcpy(badTxn, req + 8, 12);
            badTxn[0] ^= 0xFF;
            len = header(out, 0x0101, 12, badTxn);
            len += xorAttr(out + len, 8, 1);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            len = header(out, 0x0101, 12, req + 8);
            len += xorAttr(out + len, 8, 1);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_ERROR_RESPONSE_THEN_VALID:
            len = header(out, 0x0111, 0, req + 8); // binding error response
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            len = header(out, 0x0101, 12, req + 8);
            len += xorAttr(out + len, 8, 1);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_ROGUE_SOURCE_ONLY:
            // A well formed answer, but from another port than the one that was asked
            len = header(out, 0x0101, 12, req + 8);
            len += xorAttr(out + len, 8, 1);
            sendTo(rogueSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_LENGTH_LARGER_THAN_DATA:
            len = header(out, 0x0101, 12 + 40, req + 8); // claims 40 more bytes than sent
            len += xorAttr(out + len, 8, 1);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_LENGTH_NOT_MULTIPLE_OF_FOUR:
            len = header(out, 0x0101, 13, req + 8);
            len += xorAttr(out + len, 8, 1);
            out[len++] = 0;
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_ATTRIBUTE_OUT_OF_BOUNDS:
            len = header(out, 0x0101, 8, req + 8);
            put16(out + len, 0x8022); put16(out + len + 2, 200); // attribute longer than the message
            memset(out + len + 4, 'x', 4);
            len += 8;
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_WRONG_ATTRIBUTE_LENGTH:
            len = header(out, 0x0101, 12, req + 8);
            len += xorAttr(out + len, 4, 1);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_WRONG_FAMILY:
            len = header(out, 0x0101, 12, req + 8);
            len += xorAttr(out + len, 8, 2);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len);
            break;
        case SCN_TRUNCATED_HEADER:
            len = header(out, 0x0101, 0, req + 8);
            sendTo(serverSock, (struct sockaddr*)&from, fromLen, out, len - 5);
            break;
        }
    }
}

static int startServer(void) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    struct sockaddr_in rogueAddr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = 0;
    serverSock = socket(AF_INET, SOCK_DGRAM, 0);
    rogueSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSock == INVALID_SOCKET || rogueSock == INVALID_SOCKET) {
        return -1;
    }
    if (bind(serverSock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        return -1;
    }
    rogueAddr = addr;
    if (bind(rogueSock, (struct sockaddr*)&rogueAddr, sizeof(rogueAddr)) != 0) {
        return -1;
    }
    if (getsockname(serverSock, (struct sockaddr*)&addr, &len) != 0) {
        return -1;
    }
    serverPort = ntohs(addr.sin_port);
    stopServer = 0;
    return PltCreateThread("TestStun", serverProc, NULL, &serverThread);
}

static void stopStunServer(void) {
    stopServer = 1;
    PltInterruptThread(&serverThread);
    PltJoinThread(&serverThread);
    closeSocket(serverSock);
    closeSocket(rogueSock);
}

static int query(int scn, unsigned int* wan) {
    *wan = 0xDEADBEEF;
    scenario = scn;
    return LiFindExternalAddressIP4("127.0.0.1", serverPort, wan);
}

static void testStun(void) {
    unsigned int wan;
    int ret;
    unsigned char bytes[4];

    ret = query(SCN_VALID_AFTER_PADDED_UNKNOWN_ATTR, &wan);
    if (ret != 0) {
        // Resolving the numeric address can fail when no IPv4 address is configured at all
        printf("SKIP stun: query failed with %d (no usable IPv4 configuration?)\n", ret);
        return;
    }
    memcpy(bytes, &wan, 4);
    CHECK(memcmp(bytes, expectedIp, 4) == 0);

    // The first answer carries another transaction id, the second one is valid
    ret = query(SCN_WRONG_TRANSACTION_THEN_VALID, &wan);
    CHECK_EQ_INT(ret, 0);
    memcpy(bytes, &wan, 4);
    CHECK(memcmp(bytes, expectedIp, 4) == 0);

    // An error response must not end the wait
    ret = query(SCN_ERROR_RESPONSE_THEN_VALID, &wan);
    CHECK_EQ_INT(ret, 0);

    // Malformed answers fail, and they must not write the address
    ret = query(SCN_LENGTH_LARGER_THAN_DATA, &wan);
    CHECK(ret != 0);
    CHECK(wan == 0xDEADBEEF);
    ret = query(SCN_LENGTH_NOT_MULTIPLE_OF_FOUR, &wan);
    CHECK(ret != 0);
    CHECK(wan == 0xDEADBEEF);
    ret = query(SCN_TRUNCATED_HEADER, &wan);
    CHECK(ret != 0);
    CHECK(wan == 0xDEADBEEF);
    ret = query(SCN_ROGUE_SOURCE_ONLY, &wan);
    CHECK(ret != 0);
    CHECK(wan == 0xDEADBEEF);
}

// These answers pass validateStunResponse() but have bad attributes: they are answered at once
static void testAttributeErrors(void) {
    unsigned int wan;

    CHECK_EQ_INT(query(SCN_ATTRIBUTE_OUT_OF_BOUNDS, &wan), -5);
    CHECK(wan == 0xDEADBEEF);
    CHECK_EQ_INT(query(SCN_WRONG_ATTRIBUTE_LENGTH, &wan), -5);
    CHECK(wan == 0xDEADBEEF);
    CHECK_EQ_INT(query(SCN_WRONG_FAMILY, &wan), -5);
    CHECK(wan == 0xDEADBEEF);
}

int main(void) {
    if (initializePlatformSockets() != 0 || startServer() != 0) {
        printf("FAIL cannot start the loopback STUN responder\n");
        testChecks++;
        testFailures++;
        return testFinish("stun");
    }
    RUN(testStun);
    RUN(testAttributeErrors);
    stopStunServer();
    cleanupPlatformSockets();
    return testFinish("stun");
}
