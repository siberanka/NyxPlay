package com.limelight;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.EOFException;
import java.io.IOException;
import java.net.ConnectException;
import java.net.SocketTimeoutException;

import javax.net.ssl.SSLException;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class PairingErrorMessageTest {
    @Test
    public void connectionLoss_mapsToReadableMessage() {
        Integer expected = R.string.pair_host_stopped;
        assertEquals(expected, PcView.getPairingErrorMessageId(new SocketTimeoutException("timeout")));
        assertEquals(expected, PcView.getPairingErrorMessageId(new EOFException()));
        assertEquals(expected, PcView.getPairingErrorMessageId(new ConnectException("refused")));
        assertEquals(expected, PcView.getPairingErrorMessageId(new SSLException("closed")));
        assertEquals(expected, PcView.getPairingErrorMessageId(new IOException("unexpected end of stream on http://10.0.0.5:47989/...")));
    }

    @Test
    public void otherErrors_keepTheirOwnText() {
        assertNull(PcView.getPairingErrorMessageId(new IOException("Response too large")));
    }
}
