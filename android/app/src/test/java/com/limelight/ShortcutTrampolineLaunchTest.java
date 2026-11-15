package com.limelight;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Looper;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.utils.LaunchToken;

import org.junit.After;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowAlertDialog;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class ShortcutTrampolineLaunchTest {
    private static final String HOST_UUID = "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f";

    private Application app;
    private File tempDir;

    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    @Before
    public void setUp() {
        app = ApplicationProvider.getApplicationContext();
        tempDir = new File(app.getCacheDir(), "art-test");
        tempDir.mkdirs();
    }

    @After
    public void tearDown() {
        File[] files = tempDir.listFiles();
        if (files != null) {
            for (File f : files) {
                f.delete();
            }
        }
        tempDir.delete();
    }

    private Intent shortcutIntent(String uuid, String token) {
        Intent intent = new Intent(app, ShortcutTrampoline.class);
        intent.setAction(Intent.ACTION_DEFAULT);
        intent.putExtra(AppView.UUID_EXTRA, uuid);
        intent.putExtra(AppView.NAME_EXTRA, "Desk");
        if (token != null) {
            intent.putExtra(LaunchToken.EXTRA, token);
        }
        return intent;
    }

    private ActivityController<ShortcutTrampoline> launch(Intent intent) {
        ActivityController<ShortcutTrampoline> controller = Robolectric.buildActivity(ShortcutTrampoline.class, intent);
        controller.create().start().resume();
        shadowOf(Looper.getMainLooper()).idle();

        // Shortcut files are read on a worker thread, so wait for the dialog it ends up showing
        if (Intent.ACTION_VIEW.equals(intent.getAction())) {
            long deadline = System.currentTimeMillis() + 5000;
            while (ShadowAlertDialog.getLatestAlertDialog() == null && System.currentTimeMillis() < deadline) {
                try {
                    Thread.sleep(10);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
                shadowOf(Looper.getMainLooper()).idle();
            }
        }
        return controller;
    }

    private int boundConnections() {
        return shadowOf(app).getBoundServiceConnections().size();
    }

    private File writeArt(String name, byte[] content) throws Exception {
        File f = new File(tempDir, name);
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(content);
        }
        return f;
    }

    private Intent viewIntent(File art) throws Exception {
        Uri uri = Uri.parse("content://shortcut.test/" + art.getName());
        shadowOf(app.getContentResolver()).registerInputStream(uri, new java.io.FileInputStream(art));
        Intent intent = new Intent(app, ShortcutTrampoline.class);
        intent.setAction(Intent.ACTION_VIEW);
        intent.setData(uri);
        return intent;
    }

    private void storeHostAndApps(String hostName) throws Exception {
        com.limelight.nvstream.http.ComputerDetails details = new com.limelight.nvstream.http.ComputerDetails();
        details.uuid = HOST_UUID;
        details.name = hostName;
        details.localAddress = new com.limelight.nvstream.http.ComputerDetails.AddressTuple("10.0.0.5", 47989);
        new com.limelight.computers.ComputerDatabaseManager(app).updateComputer(details);

        String xml = "<root status_code=\"200\"><App><AppTitle>Real Game</AppTitle><ID>5</ID></App></root>";
        try (java.io.OutputStream out = com.limelight.utils.CacheHelper.openCacheFileForOutput(app.getCacheDir(), "applist", HOST_UUID)) {
            out.write(xml.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String message(AlertDialog dialog) {
        return String.valueOf(shadowOf(dialog).getMessage());
    }

    @Test
    public void intentWithoutTokenAsksBeforeStarting() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent(HOST_UUID, null));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertTrue(dialog.isShowing());
        assertEquals(app.getString(R.string.launch_confirm_title), String.valueOf(shadowOf(dialog).getTitle()));
        assertEquals(0, boundConnections());
        assertFalse(c.get().isFinishing());
    }

    @Test
    public void intentWithWrongTokenAsksBeforeStarting() {
        launch(shortcutIntent(HOST_UUID, "not-the-token"));

        assertNotNull(ShadowAlertDialog.getLatestAlertDialog());
        assertEquals(0, boundConnections());
    }

    @Test
    public void intentWithTokenStartsWithoutPrompt() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent(HOST_UUID, LaunchToken.get(app)));

        // Only the progress spinner is shown, never the confirmation
        AlertDialog latest = ShadowAlertDialog.getLatestAlertDialog();
        if (latest != null) {
            assertFalse(app.getString(R.string.launch_confirm_title).contentEquals(shadowOf(latest).getTitle()));
        }
        assertEquals(1, boundConnections());
        assertFalse(c.get().isFinishing());
    }

    @Test
    public void proceedStartsTheLaunch() {
        launch(shortcutIntent(HOST_UUID, null));
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();

        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();

        assertEquals(1, boundConnections());
    }

    @Test
    public void cancelFinishesWithoutStarting() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent(HOST_UUID, null));
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();

        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();

        assertTrue(c.get().isFinishing());
        assertEquals(0, boundConnections());
    }

    @Test
    public void invalidHostIdShowsErrorAndNeverBinds() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent("../../x", LaunchToken.get(app)));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.scut_invalid_uuid), message(dialog));
        assertEquals(0, boundConnections());

        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(c.get().isFinishing());
    }

    @Test
    public void invalidAppIdShowsErrorAndNeverBinds() {
        Intent intent = shortcutIntent(HOST_UUID, LaunchToken.get(app));
        intent.putExtra(Game.EXTRA_APP_ID, "abc");
        launch(intent);

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.scut_invalid_app_id), message(dialog));
        assertEquals(0, boundConnections());
    }

    @Test
    public void oversizedShortcutFileIsRejectedWithOneError() throws Exception {
        byte[] huge = new byte[1024 * 1024];
        Arrays.fill(huge, (byte) 'a');
        launch(viewIntent(writeArt("huge.art", huge)));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.scut_art_read_error), message(dialog));
        assertEquals(1, ShadowAlertDialog.getShownDialogs().size());
        assertEquals(0, boundConnections());
    }

    @Test
    public void malformedShortcutFileIsRejectedWithOneError() throws Exception {
        launch(viewIntent(writeArt("bad.art", "this is not a shortcut\n".getBytes(StandardCharsets.UTF_8))));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.scut_art_read_error), message(dialog));
        assertEquals(1, ShadowAlertDialog.getShownDialogs().size());
        assertEquals(0, boundConnections());
    }

    @Test
    public void validShortcutFileAsksAndNamesAppAndComputer() throws Exception {
        storeHostAndApps("Desk");
        String art = "# test\n[host_uuid] " + HOST_UUID + "\n[host_name] Desk\n[app_name] Game One\n[app_id] 5\n";
        launch(viewIntent(writeArt("ok.art", art.getBytes(StandardCharsets.UTF_8))));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.launch_confirm_title), String.valueOf(shadowOf(dialog).getTitle()));
        assertTrue(message(dialog).contains("Real Game"));
        assertTrue(message(dialog).contains("Desk"));
        assertEquals(0, boundConnections());
    }

    @Test
    public void shortcutFileUsingTheFileSchemeIsRejected() throws Exception {
        File art = writeArt("scheme.art", ("[host_uuid] " + HOST_UUID + "\n").getBytes(StandardCharsets.UTF_8));
        Intent intent = new Intent(app, ShortcutTrampoline.class);
        intent.setAction(Intent.ACTION_VIEW);
        intent.setData(Uri.fromFile(art));
        launch(intent);

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.scut_art_read_error), message(dialog));
        assertEquals(0, boundConnections());
    }

    @Test
    public void confirmationNamesComeFromStoredRecordsNotFromTheFile() throws Exception {
        storeHostAndApps("Living Room");
        String art = "[host_uuid] " + HOST_UUID + "\n[host_name] Work PC\n[app_name] Notepad\n[app_id] 5\n";
        launch(viewIntent(writeArt("spoof.art", art.getBytes(StandardCharsets.UTF_8))));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        String text = message(dialog);
        assertTrue(text, text.contains("Real Game"));
        assertTrue(text, text.contains("Living Room"));
        assertFalse(text, text.contains("Work PC"));
        assertFalse(text, text.contains("Notepad"));
    }

    @Test
    public void unknownHostGetsTheGenericConfirmation() throws Exception {
        String art = "[host_uuid] " + HOST_UUID + "\n[host_name] Work PC\n[app_name] Notepad\n[app_id] 5\n";
        launch(viewIntent(writeArt("unknown.art", art.getBytes(StandardCharsets.UTF_8))));

        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals(app.getString(R.string.launch_confirm_message), message(dialog));
    }

    @Test
    public void viewIntentAsksEvenWhenItCarriesTheToken() throws Exception {
        String art = "[host_uuid] " + HOST_UUID + "\n";
        Intent intent = viewIntent(writeArt("tok.art", art.getBytes(StandardCharsets.UTF_8)));
        intent.putExtra(LaunchToken.EXTRA, LaunchToken.get(app));
        launch(intent);

        assertNotNull(ShadowAlertDialog.getLatestAlertDialog());
        assertEquals(0, boundConnections());
    }

    @Test
    public void leavingTheActivityReleasesTheServiceOnce() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent(HOST_UUID, LaunchToken.get(app)));
        assertEquals(1, boundConnections());

        c.pause().stop();
        assertEquals(0, boundConnections());

        // A second release path must not throw
        c.destroy();
        assertEquals(0, boundConnections());
    }

    @Test
    public void leavingWhileTheConfirmationIsShownDismissesIt() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent(HOST_UUID, null));
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertTrue(dialog.isShowing());

        c.pause().stop();

        assertFalse(dialog.isShowing());
        assertTrue(c.get().isFinishing());
    }

    @Test
    public void recreateWithConfirmationShownDoesNotCrash() {
        ActivityController<ShortcutTrampoline> c = launch(shortcutIntent(HOST_UUID, null));
        c.recreate();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(0, boundConnections());
    }

    @Test
    public void tokenIsStableAndPrivate() {
        Context context = app;
        String first = LaunchToken.get(context);
        assertEquals(48, first.length());
        assertEquals(first, LaunchToken.get(context));
        assertTrue(LaunchToken.matches(context, first));
        assertFalse(LaunchToken.matches(context, null));
        assertFalse(LaunchToken.matches(context, ""));
        assertFalse(LaunchToken.matches(context, first.substring(1)));
    }
}
