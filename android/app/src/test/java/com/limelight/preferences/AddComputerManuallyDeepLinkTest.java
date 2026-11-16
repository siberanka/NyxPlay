package com.limelight.preferences;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.net.Uri;
import android.os.Looper;
import android.widget.TextView;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.AppView;
import com.limelight.Game;
import com.limelight.R;
import com.limelight.ShortcutTrampoline;
import com.limelight.TestLogSuppressor;
import com.limelight.utils.LaunchToken;

import org.junit.After;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowAlertDialog;

import java.util.ArrayList;
import java.util.List;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class AddComputerManuallyDeepLinkTest {
    private static final String HOST_UUID = "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f";

    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    private final List<ActivityController<AddComputerManually>> opened = new ArrayList<>();

    @After
    public void closeActivities() {
        // Destroying the activities stops the background add thread, so no work from one test
        // can show dialogs or post to the looper while the next test runs
        for (ActivityController<AddComputerManually> controller : opened) {
            try {
                controller.pause().stop().destroy();
            } catch (RuntimeException ignored) {
                // The activity may already be destroyed
            }
        }
        opened.clear();
        shadowOf(Looper.getMainLooper()).idle();
    }

    private ActivityController<AddComputerManually> open(String uri) {
        Application app = ApplicationProvider.getApplicationContext();
        Intent intent = new Intent(app, AddComputerManually.class);
        intent.setAction(Intent.ACTION_VIEW);
        intent.setData(Uri.parse(uri));
        ActivityController<AddComputerManually> controller = Robolectric.buildActivity(AddComputerManually.class, intent);
        controller.create().start().resume();
        shadowOf(Looper.getMainLooper()).idle();
        opened.add(controller);
        return controller;
    }

    @Test
    public void launchLinkHandsOffWithoutTokenSoTheTrampolineAsks() {
        ActivityController<AddComputerManually> c = open("art://launch?host_uuid=" + HOST_UUID + "&app_id=1&host_name=Work%20PC");

        // The confirmation is shown by the trampoline, with names from the app's own records
        assertNull(ShadowAlertDialog.getLatestAlertDialog());

        Intent started = shadowOf(c.get()).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(ShortcutTrampoline.class.getName(), started.getComponent().getClassName());
        assertEquals(HOST_UUID, started.getStringExtra(AppView.UUID_EXTRA));
        assertEquals("1", started.getStringExtra(Game.EXTRA_APP_ID));
        assertNull(started.getStringExtra(LaunchToken.EXTRA));
        assertTrue(c.get().isFinishing());
    }

    @Test
    public void invalidHostIdInLaunchLinkStillShowsTheErrorAfterTheLinkScreenCloses() {
        ActivityController<AddComputerManually> c = open("art://launch?host_uuid=..%2Fevil&app_id=1");
        Intent started = shadowOf(c.get()).getNextStartedActivity();
        assertNotNull(started);

        // The trampoline shows its message while the link screen is still being torn down
        ActivityController<ShortcutTrampoline> trampoline =
                Robolectric.buildActivity(ShortcutTrampoline.class, started);
        trampoline.create().start().resume();
        shadowOf(Looper.getMainLooper()).idle();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertTrue(dialog.isShowing());

        c.pause().stop().destroy();
        shadowOf(Looper.getMainLooper()).idle();

        assertTrue("the error dialog was closed", dialog.isShowing());
        assertEquals(trampoline.get().getString(R.string.scut_invalid_uuid), String.valueOf(shadowOf(dialog).getMessage()));

        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(trampoline.get().isFinishing());
        trampoline.pause().stop().destroy();
    }

    @Test
    public void pairConfirmationShowsTheAddressNotTheSuppliedName() {
        open("art://10.0.2.2:57989?pin=1234&passphrase=abcd&name=Trusted%20Bank");

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        String text = String.valueOf(shadowOf(dialog).getMessage());
        assertTrue(text, text.contains("10.0.2.2:57989"));
        assertFalse(text, text.contains("Trusted Bank"));
    }

    @Test
    public void pairLinkKeepsTheActivityAliveAfterProceed() {
        ActivityController<AddComputerManually> c = open("art://10.0.2.2:57989?pin=1234&passphrase=abcd&name=T");

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(c.get().getString(R.string.pair_pc_confirm_title), String.valueOf(shadowOf(dialog).getTitle()));

        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();

        // Closing here would interrupt the add before it completes
        assertFalse(c.get().isFinishing());
    }

    @Test
    public void linkWithoutQueryPrefillsTheHostAndDoesNotAsk() {
        ActivityController<AddComputerManually> c = open("art://10.0.2.2:57989");

        assertNull(ShadowAlertDialog.getLatestAlertDialog());
        TextView host = c.get().findViewById(R.id.hostTextView);
        assertEquals("10.0.2.2:57989", host.getText().toString());
    }

    @Test
    public void ipv6LinkPrefillsTheHost() {
        ActivityController<AddComputerManually> c = open("art://[::1]");

        assertNull(ShadowAlertDialog.getLatestAlertDialog());
        TextView host = c.get().findViewById(R.id.hostTextView);
        assertEquals("[::1]", host.getText().toString());
    }
}
