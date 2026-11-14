package com.limelight;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.app.Application;
import android.content.Intent;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.utils.LaunchToken;

import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.lang.reflect.Field;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class PcViewPairingExtrasTest {
    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    private static Object field(PcView view, String name) throws Exception {
        Field f = PcView.class.getDeclaredField(name);
        f.setAccessible(true);
        return f.get(view);
    }

    private PcView open(String token) {
        Application app = ApplicationProvider.getApplicationContext();
        Intent intent = new Intent(app, PcView.class);
        intent.putExtra("hostname", "10.0.2.2");
        intent.putExtra("port", 57989);
        intent.putExtra("pin", "1111");
        intent.putExtra("passphrase", "abcd");
        if (token != null) {
            intent.putExtra(LaunchToken.EXTRA, token);
        }
        return Robolectric.buildActivity(PcView.class, intent).create().get();
    }

    @Test
    public void externalPairingRequestIsNotTrusted() throws Exception {
        PcView view = open(null);

        assertEquals(Boolean.FALSE, field(view, "pendingPairingTrusted"));
        assertTrue(field(view, "pendingPairingAddress") != null);
    }

    @Test
    public void requestWithWrongTokenIsNotTrusted() throws Exception {
        PcView view = open("guess");

        assertEquals(Boolean.FALSE, field(view, "pendingPairingTrusted"));
    }

    @Test
    public void inAppHandOffIsTrusted() throws Exception {
        Application app = ApplicationProvider.getApplicationContext();
        PcView view = open(LaunchToken.get(app));

        assertEquals(Boolean.TRUE, field(view, "pendingPairingTrusted"));
    }

    @Test
    public void secretsAreRemovedFromTheIntent() {
        PcView view = open(null);

        assertFalse(view.getIntent().hasExtra("pin"));
        assertFalse(view.getIntent().hasExtra("passphrase"));
        assertNull(view.getIntent().getStringExtra(LaunchToken.EXTRA));
    }
}
