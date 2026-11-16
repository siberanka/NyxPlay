package com.limelight;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class GameRtspHintTest {
    @Test
    public void timeoutDuringRtspIsNotReportedAsClosedConnection() {
        assertEquals(R.string.conn_error_rtsp_timeout, Game.rtspFailureHint("RTSP handshake", 110));
        assertEquals(R.string.conn_error_rtsp_timeout, Game.rtspFailureHint("RTSP handshake", 10060));
    }

    @Test
    public void resetDuringRtspKeepsTheClosedConnectionHint() {
        assertEquals(R.string.conn_error_rtsp_reset, Game.rtspFailureHint("RTSP handshake", 104));
        assertEquals(R.string.conn_error_rtsp_reset, Game.rtspFailureHint("RTSP handshake", 10054));
    }

    @Test
    public void otherStagesAndErrorsGetNoRtspHint() {
        assertEquals(0, Game.rtspFailureHint("video stream", 110));
        assertEquals(0, Game.rtspFailureHint("RTSP handshake", 404));
        assertEquals(0, Game.rtspFailureHint(null, 104));
    }

    @Test
    public void timeoutMessageSaysTheHostDidNotAnswer() {
        String text = androidx.test.core.app.ApplicationProvider.getApplicationContext()
                .getString(R.string.conn_error_rtsp_timeout);
        org.junit.Assert.assertFalse(text, text.toLowerCase(java.util.Locale.ROOT).contains("closed"));
    }
}
