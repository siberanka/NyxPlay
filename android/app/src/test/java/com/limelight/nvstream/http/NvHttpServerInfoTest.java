package com.limelight.nvstream.http;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;
import static org.mockito.Mockito.mock;

import com.limelight.TestHostResponses;

import okhttp3.OkHttpClient;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.mockwebserver.SocketPolicy;
import okio.Buffer;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.lang.reflect.Field;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class NvHttpServerInfoTest {
    private static final int CAP = 4 * 1024 * 1024;

    private MockWebServer server;

    @Before
    public void setUp() throws IOException {
        server = new MockWebServer();
        server.start();
    }

    @After
    public void tearDown() {
        try {
            server.shutdown();
        } catch (IOException ignored) {
            // a response that was cancelled mid-delay can keep the server busy
        }
    }

    private NvHTTP newHttp() throws IOException {
        return new NvHTTP(new ComputerDetails.AddressTuple(server.getHostName(), server.getPort()), 0,
                "0123456789ABCDEF", null, mock(LimelightCryptoProvider.class));
    }

    @Test
    public void serverinfo_exactlyCap_accepted() throws Exception {
        String body = TestHostResponses.serverInfoOfSize("3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f", CAP);
        assertEquals(CAP, body.length());
        server.enqueue(new MockResponse().setBody(body));
        assertEquals(CAP, newHttp().getServerInfo(true).length());
    }

    @Test
    public void serverinfo_capPlusOne_rejected() throws Exception {
        server.enqueue(new MockResponse().setBody(TestHostResponses.serverInfoOfSize("abc", CAP + 1)));
        try {
            newHttp().getServerInfo(true);
            fail("Expected IOException");
        } catch (IOException expected) {
            // expected
        }
    }

    @Test
    public void serverinfo_chunkedOverCap_rejected() throws Exception {
        Buffer buffer = new Buffer();
        buffer.writeUtf8(TestHostResponses.serverInfoOfSize("abc", CAP + 1));
        server.enqueue(new MockResponse().setChunkedBody(buffer, 8192));
        try {
            newHttp().getServerInfo(true);
            fail("Expected IOException");
        } catch (IOException expected) {
            // expected
        }
    }

    @Test
    public void getComputerDetails_invalidUniqueid_throwsXmlPullParser() throws Exception {
        NvHTTP http = newHttp();
        StringBuilder longId = new StringBuilder();
        for (int i = 0; i < 65; i++) {
            longId.append('a');
        }
        String[] bad = { "../../x", "", longId.toString() };
        for (String id : bad) {
            try {
                http.getComputerDetails(TestHostResponses.serverInfo(id));
                fail("Accepted uniqueid: " + id);
            } catch (XmlPullParserException expected) {
                // expected
            }
        }
    }

    @Test
    public void getComputerDetails_commonUuidFormats_ok() throws Exception {
        NvHTTP http = newHttp();
        String[] good = { "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f", "0123456789ABCDEF", "0123456789abcdef0123456789abcdef" };
        for (String id : good) {
            ComputerDetails details = http.getComputerDetails(TestHostResponses.serverInfo(id));
            assertEquals(id, details.uuid);
            assertEquals(ComputerDetails.State.ONLINE, details.state);
        }
    }

    @Test
    public void clients_timeouts() throws Exception {
        NvHTTP http = newHttp();
        OkHttpClient noTimeout = getClient(http, "httpClientLongConnectNoReadTimeout");
        // Launch and resume may stay quiet while the host starts the app, but the call is bounded
        assertEquals(NvHTTP.LAUNCH_CALL_TIMEOUT, noTimeout.callTimeoutMillis());
        assertEquals(0, noTimeout.readTimeoutMillis());
        OkHttpClient cancel = getClient(http, "httpClientCancel");
        assertEquals(NvHTTP.CANCEL_CALL_TIMEOUT, cancel.callTimeoutMillis());
        assertTrue(cancel.readTimeoutMillis() > 0);
        OkHttpClient normal = getClient(http, "httpClientLongConnectTimeout");
        assertEquals(NvHTTP.CALL_TIMEOUT, normal.callTimeoutMillis());
        OkHttpClient pairing = getClient(http, "httpClientPairingWait");
        assertEquals(NvHTTP.PAIRING_WAIT_TIMEOUT, pairing.callTimeoutMillis());
        assertTrue(pairing.callTimeoutMillis() > 0);
    }

    private static OkHttpClient getClient(NvHTTP http, String field) throws Exception {
        Field f = NvHTTP.class.getDeclaredField(field);
        f.setAccessible(true);
        return (OkHttpClient) f.get(http);
    }

    @Test
    public void errorResponse_thenSuccess_notLeaked() throws Exception {
        server.enqueue(new MockResponse().setResponseCode(500).setBody("oops"));
        server.enqueue(new MockResponse().setBody(TestHostResponses.serverInfo("abc")));
        NvHTTP http = newHttp();
        try {
            http.getServerInfo(true);
            fail("Expected HostHttpResponseException");
        } catch (HostHttpResponseException expected) {
            assertEquals(500, expected.getErrorCode());
        }
        assertNotNull(http.getServerInfo(true));
    }

    @Test
    public void reset_midResponse_isIOException_notRuntime() throws Exception {
        SocketPolicy[] policies = { SocketPolicy.DISCONNECT_AT_START, SocketPolicy.DISCONNECT_DURING_RESPONSE_BODY };
        for (SocketPolicy policy : policies) {
            server.enqueue(new MockResponse().setBody(TestHostResponses.serverInfo("abc")).setSocketPolicy(policy));
            try {
                newHttp().getServerInfo(true);
                fail("Expected IOException for " + policy);
            } catch (IOException expected) {
                // expected
            }
        }
    }

    @Test
    public void cancelPairing_abortsBlockedCall() throws Exception {
        server.enqueue(new MockResponse().setBody("<root status_code=\"200\"><paired>1</paired></root>")
                .setBodyDelay(30, TimeUnit.SECONDS));
        final NvHTTP http = newHttp();
        final AtomicReference<Throwable> result = new AtomicReference<>();
        Thread t = new Thread(() -> {
            try {
                http.executePairingCommand("phrase=getservercert", false);
                result.set(new AssertionError("Call returned normally"));
            } catch (Throwable e) {
                result.set(e);
            }
        });
        t.start();
        Thread.sleep(500);
        http.cancelPairing();
        t.join(5000);
        assertTrue("Pairing call still blocked", !t.isAlive());
        assertTrue(String.valueOf(result.get()), result.get() instanceof IOException);
    }

    @Test
    public void pair_cancelledBeforeStart_failsImmediately() throws Exception {
        server.enqueue(new MockResponse().setBody("<root status_code=\"200\"><paired>1</paired></root>")
                .setBodyDelay(30, TimeUnit.SECONDS));
        NvHTTP http = newHttp();
        http.cancelPairing();
        long start = System.currentTimeMillis();
        try {
            http.executePairingCommand("phrase=getservercert", false);
            fail("Expected IOException");
        } catch (IOException expected) {
            // expected
        }
        assertTrue(System.currentTimeMillis() - start < 5000);
    }
}
