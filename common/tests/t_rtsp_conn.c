// RTSP over TCP: response size cap (upstream 7b026e7) and the NyxPlay resilience changes
// (bounded retry of idempotent requests after a reset before any response byte, completeness
// check of plain responses after a reset). A loopback TCP server plays the host.
//
// This file includes RtspConnection.c to reach its static functions.

#include "testutil.h"

#include "RtspConnection.c"

#include <stdarg.h>

typedef enum {
    SCN_OK,
    SCN_RESET_FIRST_THEN_OK,
    SCN_ALWAYS_RESET,
    SCN_OVERSIZE,
    SCN_COMPLETE_THEN_RESET,
    SCN_HEADERS_UNTERMINATED_THEN_RESET,
    SCN_SHORT_BODY_THEN_RESET,
    SCN_CLOSE_WITHOUT_DATA_FIRST_THEN_OK,
} SCENARIO;

static SOCKET listenSock = INVALID_SOCKET;
static volatile int stopServer;
static volatile int scenario;
static volatile int connections;
static PLT_THREAD serverThread;

static void logToStdout(const char* format, ...) {
    va_list ap;
    if (getenv("MCC_TEST_VERBOSE") == NULL) {
        return;
    }
    va_start(ap, format);
    printf("  lib: ");
    vprintf(format, ap);
    va_end(ap);
}

static const char okResponse[] = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-length: 4\r\n\r\nabcd";

static void resetClose(SOCKET s) {
    struct linger l;
    l.l_onoff = 1;
    l.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&l, sizeof(l));
    closeSocket(s);
}

// Closes after the client had time to read the response, like a host that closes the connection
// after its answer. An immediate close could turn into a reset that discards the response.
static void gracefulClose(SOCKET s) {
    PltSleepMs(150);
    closeSocket(s);
}

static int readRequest(SOCKET s) {
    char buf[2048];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n") != NULL) {
            break;
        }
    }
    return total;
}

static void serverProc(void* context) {
    (void)context;

    while (!stopServer) {
        SOCKET c;
        int idx, got;

        c = accept(listenSock, NULL, NULL);
        if (c == INVALID_SOCKET || stopServer) {
            if (c != INVALID_SOCKET) {
                closeSocket(c);
            }
            if (stopServer) {
                break;
            }
            continue;
        }
        // Only connections that carried a request count: a connection that dies before sending is
        // a probe, not an attempt of the client to send the request again
        got = readRequest(c);
        if (got <= 0) {
            logToStdout("server: empty connection (%d)\n", LastSocketError());
            closeSocket(c);
            continue;
        }
        {
            // Never inherit a linger setting: graceful close unless a scenario asks for a reset
            struct linger off;
            off.l_onoff = 0;
            off.l_linger = 0;
            setsockopt(c, SOL_SOCKET, SO_LINGER, (const char*)&off, sizeof(off));
        }
        {
            struct pollfd pf;
            char extra[64];
            int more = 0;
            pf.fd = c;
            pf.events = POLLIN;
            PltSleepMs(20);
            if (pollSockets(&pf, 1, 0) > 0) {
                more = recv(c, extra, sizeof(extra), 0);
            }
            logToStdout("server: %d extra byte(s) after the request\n", more);
        }
        idx = connections++;
        logToStdout("server: request %d, %d bytes, scenario %d\n", idx, got, scenario);

        switch (scenario) {
        case SCN_OK:
            {
                int sent = send(c, okResponse, (int)sizeof(okResponse) - 1, 0);
                logToStdout("server: sent %d (error %d)\n", sent, LastSocketError());
            }
            gracefulClose(c);
            break;
        case SCN_RESET_FIRST_THEN_OK:
            if (idx == 0) {
                resetClose(c);
            }
            else {
                send(c, okResponse, (int)sizeof(okResponse) - 1, 0);
                closeSocket(c);
            }
            break;
        case SCN_CLOSE_WITHOUT_DATA_FIRST_THEN_OK:
            if (idx == 0) {
                gracefulClose(c);
            }
            else {
                send(c, okResponse, (int)sizeof(okResponse) - 1, 0);
                closeSocket(c);
            }
            break;
        case SCN_ALWAYS_RESET:
            resetClose(c);
            break;
        case SCN_OVERSIZE: {
            static char chunk[8192];
            memset(chunk, 'a', sizeof(chunk));
            for (int i = 0; i < 2 * 1024 * 1024 / (int)sizeof(chunk) + 64; i++) {
                if (send(c, chunk, (int)sizeof(chunk), 0) <= 0) {
                    break;
                }
            }
            closeSocket(c);
            break;
        }
        case SCN_COMPLETE_THEN_RESET:
            send(c, okResponse, (int)sizeof(okResponse) - 1, 0);
            PltSleepMs(50);
            resetClose(c);
            break;
        case SCN_HEADERS_UNTERMINATED_THEN_RESET:
            send(c, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n", 25, 0);
            PltSleepMs(50);
            resetClose(c);
            break;
        case SCN_SHORT_BODY_THEN_RESET:
            send(c, "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-length: 10\r\n\r\nabcd", 53, 0);
            PltSleepMs(50);
            resetClose(c);
            break;
        }
    }
}

static int startServer(void) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    struct sockaddr_in* remote = (struct sockaddr_in*)&RemoteAddr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET ||
            bind(listenSock, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
            listen(listenSock, 8) != 0 ||
            getsockname(listenSock, (struct sockaddr*)&addr, &len) != 0) {
        return -1;
    }

    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    remote->sin_family = AF_INET;
    remote->sin_addr.s_addr = htonl(0x7F000001);
    AddrLen = sizeof(*remote);
    RtspPortNumber = ntohs(addr.sin_port);
    stopServer = 0;
    return PltCreateThread("TestRtsp", serverProc, NULL, &serverThread);
}

static void stopRtspServer(void) {
    SOCKET wake;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    stopServer = 1;
    // Wake the blocking accept()
    getsockname(listenSock, (struct sockaddr*)&addr, &len);
    wake = socket(AF_INET, SOCK_STREAM, 0);
    connect(wake, (struct sockaddr*)&addr, len);
    closeSocket(wake);
    PltJoinThread(&serverThread);
    closeSocket(listenSock);
}

static bool transact(const char* command, int scn, int* error, int* conns) {
    RTSP_MESSAGE request, response;
    bool ret;

    memset(&response, 0, sizeof(response));
    scenario = scn;
    connections = 0;
    createRtspRequest(&request, NULL, 0, (char*)command, "rtsp://127.0.0.1", "RTSP/1.0", 0, NULL, NULL, 0);
    ret = transactRtspMessageTcp(&request, &response, error);
    if (ret) {
        CHECK_EQ_INT(response.message.response.statusCode, 200);
        freeMessage(&response);
    }
    PltSleepMs(100); // let the server count the last connection
    *conns = connections;
    return ret;
}

static void testPlainResponseCompleteness(void) {
    // No Content-length: the headers alone are the response
    CHECK(isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n", 28));
    CHECK(!isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\nCSeq: 1\r\n", 25));
    CHECK(!isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\nCSeq: 1\r", 24));
    CHECK(!isPlaintextRtspResponseComplete("", 0));
    // With Content-length the body must be there in full (any case of the header name)
    CHECK(isPlaintextRtspResponseComplete(okResponse, (int)sizeof(okResponse) - 1));
    CHECK(!isPlaintextRtspResponseComplete(okResponse, (int)sizeof(okResponse) - 2));
    CHECK(isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\ncontent-LENGTH:  3\r\n\r\nabc", 42));
    CHECK(!isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\ncontent-LENGTH:  3\r\n\r\nab", 41));
    // Huge declared lengths must not overflow
    CHECK(!isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\nContent-length: 99999999999999999999\r\n\r\nab", 62));
    // The text "Content-length:" inside a value is not a header
    CHECK(isPlaintextRtspResponseComplete("RTSP/1.0 200 OK\r\nX: Content-length: 9\r\n\r\n", 42));
}

// A reset can overtake the response on some stacks, so that the client never sees it. Such an
// attempt is a failure that is repeated like any other: a transaction ends with the response, or
// only after every allowed attempt was used.
static void checkAnswered(bool ret, int conns, int minConns) {
    CHECK(ret || conns == 1 + RTSP_MAX_REQUEST_RETRIES);
    CHECK(conns >= minConns && conns <= 1 + RTSP_MAX_REQUEST_RETRIES);
}

static void testTransactions(void) {
    int err = 0, conns = 0;
    bool ret;

    ret = transact("OPTIONS", SCN_OK, &err, &conns);
    checkAnswered(ret, conns, 1);

    // Reset before a single byte of the response: idempotent requests are repeated
    ret = transact("OPTIONS", SCN_RESET_FIRST_THEN_OK, &err, &conns);
    checkAnswered(ret, conns, 2);
    ret = transact("DESCRIBE", SCN_RESET_FIRST_THEN_OK, &err, &conns);
    checkAnswered(ret, conns, 2);
    ret = transact("SETUP", SCN_RESET_FIRST_THEN_OK, &err, &conns);
    checkAnswered(ret, conns, 2);

    // ANNOUNCE and PLAY have side effects: never repeated
    CHECK(!transact("ANNOUNCE", SCN_RESET_FIRST_THEN_OK, &err, &conns));
    CHECK_EQ_INT(conns, 1);
    CHECK(!transact("PLAY", SCN_RESET_FIRST_THEN_OK, &err, &conns));
    CHECK_EQ_INT(conns, 1);

    // A host that always resets is retried a bounded number of times
    CHECK(!transact("OPTIONS", SCN_ALWAYS_RESET, &err, &conns));
    CHECK_EQ_INT(conns, 1 + RTSP_MAX_REQUEST_RETRIES);
}

static void testResetAfterResponseData(void) {
    int err = 0, conns = 0;

    // A complete response followed by a reset is accepted
    CHECK(transact("OPTIONS", SCN_COMPLETE_THEN_RESET, &err, &conns));
    CHECK_EQ_INT(conns, 1);

    // Cut off responses are failures and are not repeated (response bytes were seen)
    CHECK(!transact("OPTIONS", SCN_HEADERS_UNTERMINATED_THEN_RESET, &err, &conns));
    CHECK_EQ_INT(conns, 1);
    CHECK(!transact("OPTIONS", SCN_SHORT_BODY_THEN_RESET, &err, &conns));
    CHECK_EQ_INT(conns, 1);
}

static void testOversizedResponse(void) {
    int err = 0, conns = 0;

    CHECK(!transact("OPTIONS", SCN_OVERSIZE, &err, &conns));
    // The server keeps sending until the client gives up; a reset can overtake the size check
    CHECK(err == EMSGSIZE || isRtspConnectionResetError(err));
    CHECK_EQ_INT(conns, 1);
}

int main(void) {
    if (initializePlatformSockets() != 0 || startServer() != 0) {
        printf("FAIL cannot start the loopback RTSP server\n");
        testChecks++;
        testFailures++;
        return testFinish("rtsp_conn");
    }
    ListenerCallbacks.logMessage = logToStdout;
    encryptedRtspEnabled = false;
    useEnet = false;
    ConnectionInterrupted = false;

    RUN(testPlainResponseCompleteness);
    RUN(testTransactions);
    RUN(testResetAfterResponseData);
    RUN(testOversizedResponse);
    stopRtspServer();
    cleanupPlatformSockets();
    return testFinish("rtsp_conn");
}
