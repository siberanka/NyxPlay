package com.limelight.nvstream.http;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import com.limelight.TestHostResponses;

import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.mockwebserver.SocketPolicy;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.IOException;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class PairingManagerTest {
    private MockWebServer server;

    @Before
    public void setUp() throws IOException {
        server = new MockWebServer();
        server.start();
    }

    @After
    public void tearDown() throws IOException {
        server.shutdown();
    }

    private NvHTTP newHttp() throws IOException {
        LimelightCryptoProvider crypto = mock(LimelightCryptoProvider.class);
        when(crypto.getPemEncodedClientCertificate()).thenReturn(new byte[] { 1, 2, 3 });
        return new NvHTTP(new ComputerDetails.AddressTuple(server.getHostName(), server.getPort()), 0,
                "0123456789ABCDEF", null, crypto);
    }

    @Test
    public void pair_emptyPlainCert_returnsAlreadyInProgress() throws Exception {
        server.enqueue(new MockResponse().setBody("<root status_code=\"200\"><paired>1</paired><plaincert></plaincert></root>"));
        server.enqueue(new MockResponse().setBody("<root status_code=\"200\"></root>")); // unpair
        NvHTTP http = newHttp();
        PairingManager.PairState state = http.getPairingManager().pair(TestHostResponses.serverInfo("abc"), "1234", null);
        assertEquals(PairingManager.PairState.ALREADY_IN_PROGRESS, state);
    }

    @Test
    public void pair_hostNotPaired_returnsFailed() throws Exception {
        server.enqueue(new MockResponse().setBody("<root status_code=\"200\"><paired>0</paired></root>"));
        NvHTTP http = newHttp();
        assertEquals(PairingManager.PairState.FAILED,
                http.getPairingManager().pair(TestHostResponses.serverInfo("abc"), "1234", null));
    }

    @Test
    public void pair_hostClosesDuringGetServerCert_throwsIOException() throws Exception {
        server.enqueue(new MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START));
        NvHTTP http = newHttp();
        try {
            http.getPairingManager().pair(TestHostResponses.serverInfo("abc"), "1234", null);
            fail("Expected IOException");
        } catch (IOException expected) {
            // expected
        }
    }
}
