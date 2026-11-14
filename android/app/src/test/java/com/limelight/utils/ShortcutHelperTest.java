package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.AppView;
import com.limelight.ShortcutTrampoline;
import com.limelight.TestLogSuppressor;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;

import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.Collections;
import java.util.List;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class ShortcutHelperTest {
    private static final String HOST_UUID = "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f";

    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    private static ComputerDetails computer() {
        ComputerDetails details = new ComputerDetails();
        details.uuid = HOST_UUID;
        details.name = "Desk";
        return details;
    }

    @Test
    public void shortcutIntentsCarryTheTokenAndDefaultAction() {
        Activity activity = Robolectric.buildActivity(Activity.class).create().get();

        Intent pc = ServerHelper.createPcShortcutIntent(activity, computer());
        Intent game = ServerHelper.createAppShortcutIntent(activity, computer(),
                new NvApp("Game", "11111111-2222-3333-4444-555555555555", 5, false));

        for (Intent intent : new Intent[]{pc, game}) {
            assertEquals(Intent.ACTION_DEFAULT, intent.getAction());
            assertEquals(ShortcutTrampoline.class.getName(), intent.getComponent().getClassName());
            assertTrue(LaunchToken.matches(activity, intent.getStringExtra(LaunchToken.EXTRA)));
        }
        assertEquals(HOST_UUID, pc.getStringExtra(AppView.UUID_EXTRA));
    }

    @Test
    public void legacyShortcutGetsTheTokenOnRefresh() {
        Activity activity = Robolectric.buildActivity(Activity.class).create().get();
        Context context = ApplicationProvider.getApplicationContext();
        ShortcutManager sm = context.getSystemService(ShortcutManager.class);

        Intent legacy = new Intent(context, ShortcutTrampoline.class);
        legacy.setAction(Intent.ACTION_DEFAULT);
        legacy.putExtra(AppView.UUID_EXTRA, HOST_UUID);
        sm.addDynamicShortcuts(Collections.singletonList(new ShortcutInfo.Builder(context, HOST_UUID)
                .setIntent(legacy)
                .setShortLabel("Desk")
                .build()));
        assertFalse(LaunchToken.matches(context,
                sm.getDynamicShortcuts().get(0).getIntent().getStringExtra(LaunchToken.EXTRA)));

        new ShortcutHelper(activity).refreshShortcutTokens();

        List<ShortcutInfo> shortcuts = sm.getDynamicShortcuts();
        assertEquals(1, shortcuts.size());
        Intent refreshed = shortcuts.get(0).getIntent();
        assertNotNull(refreshed);
        assertTrue(LaunchToken.matches(context, refreshed.getStringExtra(LaunchToken.EXTRA)));
        assertEquals(HOST_UUID, refreshed.getStringExtra(AppView.UUID_EXTRA));
    }
}
