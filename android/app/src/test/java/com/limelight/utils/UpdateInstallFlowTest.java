package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.mockito.Mockito.mock;

import android.app.AlertDialog;
import android.app.GameManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.provider.Settings;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.Game;
import com.limelight.PcView;
import com.limelight.R;
import com.limelight.TestLogSuppressor;
import com.limelight.UpdateShortcutActivity;
import com.limelight.computers.ComputerManagerService;

import org.junit.After;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.shadows.ShadowToast;

import java.io.File;
import java.io.FileOutputStream;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;

import okhttp3.OkHttpClient;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class UpdateInstallFlowTest {
    private static final String VERSION = "2026.12.1.1";
    private static final String BASE = "https://github.com/siberanka/NyxPlay/releases/download/v" + VERSION + "/";
    private static final String APK = "NyxPlay-" + VERSION + "-x86_64-release.apk";
    private static final String PAGE = "https://github.com/siberanka/NyxPlay/releases/tag/v" + VERSION;

    private final AtomicInteger fetches = new AtomicInteger();
    private volatile UpdateChecker.Result next;
    private Context context;
    private Runnable queuedInstall;
    private UpdateInstallerTest.FakeBackend backend;

    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    private static UpdateChecker.Result installable() {
        return UpdateChecker.Result.update(VERSION, "Fixes.", BASE + APK, PAGE, APK, BASE + "SHA256SUMS.txt", BASE + "SHA256SUMS.txt.sig");
    }

    @Before
    public void setUp() {
        context = ApplicationProvider.getApplicationContext();
        context.getSharedPreferences("GlPreferences", 0).edit()
                .putString("Renderer", "TestRenderer")
                .putString("Fingerprint", Build.FINGERPRINT)
                .commit();
        org.robolectric.shadows.ShadowApplication shadowApp = Shadows.shadowOf((android.app.Application) context);
        shadowApp.setSystemService(Context.GAME_SERVICE, mock(GameManager.class));
        ComputerManagerService.ComputerManagerBinder binder = mock(ComputerManagerService.ComputerManagerBinder.class);
        shadowApp.setComponentNameAndServiceForBindService(new ComponentName(context, ComputerManagerService.class), binder);
        context.getSharedPreferences("update_check", Context.MODE_PRIVATE).edit().clear().commit();

        next = installable();
        backend = new UpdateInstallerTest.FakeBackend();
        queuedInstall = null;
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                queuedInstall = command;
            }
        }, new OkHttpClient.Builder().addInterceptor(new okhttp3.Interceptor() {
            @Override
            public okhttp3.Response intercept(Chain chain) throws java.io.IOException {
                // Unit tests never reach the network
                throw new java.io.IOException("offline");
            }
        }).build(), backend, "2026.11.19.1");
        UpdateNotifier.setTestHooks(new UpdateNotifier.Fetcher() {
            @Override
            public UpdateChecker.Result fetch() {
                fetches.incrementAndGet();
                return next;
            }
        }, new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        });
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(true);
    }

    @After
    public void tearDown() {
        Game.instance = null;
        UpdateNotifier.setTestHooks(null, null);
        UpdateInstaller.setTestHooks(null, null, null, null);
    }

    // ---- helpers ----

    private ActivityController<PcView> openHostList() {
        return Robolectric.buildActivity(PcView.class).setup();
    }

    private void idle() {
        Shadows.shadowOf(android.os.Looper.getMainLooper()).idle();
    }

    private static AlertDialog latest() {
        AlertDialog d = ShadowAlertDialog.getLatestAlertDialog();
        return d != null && d.isShowing() ? d : null;
    }

    private static String title(AlertDialog d) {
        return String.valueOf(Shadows.shadowOf(d).getTitle());
    }

    private static void click(AlertDialog d, int which) {
        d.getButton(which).performClick();
        Shadows.shadowOf(android.os.Looper.getMainLooper()).idle();
    }

    private static Button findButton(View root, String text) {
        if (root instanceof Button && text.contentEquals(((Button) root).getText())) {
            return (Button) root;
        }
        if (root instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) root;
            for (int i = 0; i < g.getChildCount(); i++) {
                Button b = findButton(g.getChildAt(i), text);
                if (b != null) {
                    return b;
                }
            }
        }
        return null;
    }

    private String str(int id) {
        return context.getString(id);
    }

    // ---- Dialog ----

    @Test
    public void installableUpdateOffersInstallAndAReleasePageAction() {
        openHostList();
        AlertDialog dialog = latest();
        assertNotNull(dialog);
        assertEquals(str(R.string.update_install), dialog.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertNotNull(dialog.getButton(AlertDialog.BUTTON_NEUTRAL));
        assertNotNull(dialog.getButton(AlertDialog.BUTTON_NEGATIVE));
        assertNotNull(findButton(dialog.getWindow().getDecorView(), str(R.string.update_release_page)));
        // The address is only needed when there is no in-app install
        assertFalse(Shadows.shadowOf(dialog).getMessage().toString().contains(BASE + APK));
    }

    @Test
    public void releasePageButtonOpensTheReleasePage() {
        ActivityController<PcView> controller = openHostList();
        AlertDialog dialog = latest();
        findButton(dialog.getWindow().getDecorView(), str(R.string.update_release_page)).performClick();
        idle();

        Intent started = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(Intent.ACTION_VIEW, started.getAction());
        assertEquals(PAGE, started.getData().toString());
        assertNull(latest());
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
    }

    @Test
    public void updateWithoutSignedListKeepsTheDownloadPageButton() {
        next = UpdateChecker.Result.update(VERSION, "", BASE + APK, PAGE);
        openHostList();
        assertEquals(str(R.string.update_download), latest().getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
    }

    // ---- Install with permission ----

    @Test
    public void installStartsTheDownloadAndShowsProgressWithCancel() {
        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);

        assertEquals(UpdateInstaller.Phase.DOWNLOADING, UpdateInstaller.getState().phase);
        AlertDialog progress = latest();
        assertNotNull(progress);
        assertEquals(context.getString(R.string.update_installing_title, VERSION), title(progress));
        assertNotNull(progress.getButton(AlertDialog.BUTTON_NEGATIVE));
        assertEquals(str(R.string.update_cancel), progress.getButton(AlertDialog.BUTTON_NEGATIVE).getText().toString());

        // Cancel
        click(progress, AlertDialog.BUTTON_NEGATIVE);
        queuedInstall.run();
        idle();
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertNull(latest());
        controller.pause();
    }

    @Test
    public void progressSurvivesTheActivityBeingRecreated() {
        ActivityController<PcView> first = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        assertNotNull(latest());

        // Rotation with recreation: the old window goes, the install keeps running
        first.pause().stop().destroy();
        assertEquals(UpdateInstaller.Phase.DOWNLOADING, UpdateInstaller.getState().phase);

        openHostList();
        idle();
        AlertDialog progress = latest();
        assertNotNull("progress shown again by the new activity", progress);
        assertEquals(context.getString(R.string.update_installing_title, VERSION), title(progress));
        assertEquals(1, fetches.get());
    }

    @Test
    public void progressTextFollowsThePhases() {
        openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        AlertDialog progress = latest();

        android.widget.TextView text = (android.widget.TextView) findText(progress.getWindow().getDecorView());
        assertNotNull(text);
        assertEquals(str(R.string.update_state_downloading), text.getText().toString());
    }

    private static View findText(View root) {
        if (root instanceof android.widget.TextView && !(root instanceof Button)
                && ((android.widget.TextView) root).getText().toString().endsWith("…")) {
            return root;
        }
        if (root instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) root;
            for (int i = 0; i < g.getChildCount(); i++) {
                View v = findText(g.getChildAt(i));
                if (v != null) {
                    return v;
                }
            }
        }
        return null;
    }

    @Test
    public void failureShowsAClearMessageWithTheReleasePage() {
        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        queuedInstall.run();
        // The worker cannot reach a real server in this test: it ends in a failure state
        idle();
        AlertDialog failure = latest();
        assertNotNull(failure);
        assertEquals(str(R.string.update_failed_title), title(failure));
        assertNotNull(failure.getButton(AlertDialog.BUTTON_POSITIVE));
        assertEquals(str(R.string.update_release_page), failure.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());

        click(failure, AlertDialog.BUTTON_POSITIVE);
        Intent started = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertEquals(PAGE, started.getData().toString());
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
    }

    @Test
    public void closeAcknowledgesTheFailure() {
        openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        queuedInstall.run();
        idle();
        click(latest(), AlertDialog.BUTTON_NEGATIVE);
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertNull(latest());
    }

    @Test
    public void confirmationFromThePackageInstallerOpensTheSystemScreen() {
        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        // Pretend the worker committed a session
        UpdateInstaller.setInstallingForTest(VERSION, 7);
        UpdateInstaller.onInstallStatus(context, android.content.pm.PackageInstaller.STATUS_PENDING_USER_ACTION, 7,
                new Intent("android.intent.action.VIEW").setPackage("com.google.android.packageinstaller"));
        idle();

        Intent started = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertNotNull(started);
        assertEquals("com.google.android.packageinstaller", started.getPackage());
        assertFalse(UpdateInstaller.getState().needsConfirmation);
    }

    // ---- Permission branches ----

    private void allowSettingsScreen(UpdateAccess.Access access) {
        ResolveInfo info = new ResolveInfo();
        info.activityInfo = new ActivityInfo();
        info.activityInfo.packageName = "com.android.settings";
        info.activityInfo.name = "Settings";
        Shadows.shadowOf(context.getPackageManager()).addResolveInfoForIntent(UpdateAccess.settingsIntent(context, access), info);
    }

    @Test
    public void missingPermissionIsExplainedThenSettingsOpenAndTheInstallContinuesOnReturn() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        allowSettingsScreen(UpdateAccess.Access.NEEDS_APP_SETTING);

        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);

        AlertDialog explain = latest();
        assertEquals(str(R.string.update_permission_title), title(explain));
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        click(explain, AlertDialog.BUTTON_POSITIVE);

        Intent settings = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertEquals(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, settings.getAction());
        assertEquals("package:" + context.getPackageName(), settings.getData().toString());

        // The user allows it and comes back
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(true);
        controller.pause().resume();
        assertEquals(UpdateInstaller.Phase.DOWNLOADING, UpdateInstaller.getState().phase);
    }

    @Test
    public void returningWithoutAllowingOffersTheUpdateAgain() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        allowSettingsScreen(UpdateAccess.Access.NEEDS_APP_SETTING);

        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        controller.pause().resume();

        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        AlertDialog again = latest();
        assertNotNull(again);
        assertEquals(str(R.string.update_available_title), title(again));
    }

    @Test
    public void declinedExplanationLeavesTheUpdateOffered() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        allowSettingsScreen(UpdateAccess.Access.NEEDS_APP_SETTING);

        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        click(latest(), AlertDialog.BUTTON_NEGATIVE);
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);

        controller.pause().resume();
        assertEquals(str(R.string.update_available_title), title(latest()));
    }

    @Test
    public void deviceWithoutTheSettingsScreenFallsBackToTheReleasePage() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        // no settings activity registered, as on some TVs
        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);

        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        Intent started = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(PAGE, started.getData().toString());
        assertEquals(str(R.string.update_install_unavailable), ShadowToast.getTextOfLatestToast());
    }

    // ---- Shortcuts ----

    @Test
    public void shortcutActionsAreValidated() {
        Intent check = new Intent(UpdateShortcuts.ACTION_CHECK);
        assertEquals(UpdateShortcuts.Action.CHECK, UpdateShortcuts.parse(check, null));

        Intent install = new Intent(UpdateShortcuts.ACTION_INSTALL).putExtra(UpdateShortcuts.EXTRA_VERSION, VERSION);
        assertEquals(UpdateShortcuts.Action.INSTALL, UpdateShortcuts.parse(install, VERSION));
        // stale shortcut for another version, or nothing pending: only a check
        assertEquals(UpdateShortcuts.Action.CHECK, UpdateShortcuts.parse(install, "2026.12.2.1"));
        assertEquals(UpdateShortcuts.Action.CHECK, UpdateShortcuts.parse(install, null));
        // missing or malformed extras
        assertEquals(UpdateShortcuts.Action.CHECK, UpdateShortcuts.parse(new Intent(UpdateShortcuts.ACTION_INSTALL), VERSION));
        assertEquals(UpdateShortcuts.Action.CHECK, UpdateShortcuts.parse(
                new Intent(UpdateShortcuts.ACTION_INSTALL).putExtra(UpdateShortcuts.EXTRA_VERSION, "../../x"), "../../x"));
        assertEquals(UpdateShortcuts.Action.CHECK, UpdateShortcuts.parse(
                new Intent(UpdateShortcuts.ACTION_INSTALL).putExtra(UpdateShortcuts.EXTRA_VERSION, 5), VERSION));
        // anything else is ignored
        assertEquals(UpdateShortcuts.Action.NONE, UpdateShortcuts.parse(new Intent(Intent.ACTION_VIEW), VERSION));
        assertEquals(UpdateShortcuts.Action.NONE, UpdateShortcuts.parse(new Intent("com.example.EVIL").putExtra(UpdateShortcuts.EXTRA_VERSION, VERSION), VERSION));
        assertEquals(UpdateShortcuts.Action.NONE, UpdateShortcuts.parse(new Intent(), VERSION));
        assertEquals(UpdateShortcuts.Action.NONE, UpdateShortcuts.parse(null, VERSION));
    }

    @Test
    public void shortcutActivityIsNotExportedAndHasNoIntentFilter() throws Exception {
        PackageManager pm = context.getPackageManager();
        ActivityInfo info = pm.getActivityInfo(new ComponentName(context, UpdateShortcutActivity.class), 0);
        assertFalse(info.exported);

        for (String action : new String[] {UpdateShortcuts.ACTION_CHECK, UpdateShortcuts.ACTION_INSTALL}) {
            List<ResolveInfo> hits = pm.queryIntentActivities(new Intent(action).setPackage(context.getPackageName()), 0);
            for (ResolveInfo r : hits) {
                assertFalse("no implicit way to reach " + r.activityInfo.name, r.activityInfo.name.endsWith("UpdateShortcutActivity"));
            }
        }
    }

    @Test
    public void manifestDeclaresTheInstallPermissionsServiceAndReceiver() throws Exception {
        PackageManager pm = context.getPackageManager();
        PackageInfo pkg = pm.getPackageInfo(context.getPackageName(), PackageManager.GET_PERMISSIONS);
        List<String> perms = java.util.Arrays.asList(pkg.requestedPermissions);
        assertTrue(perms.contains("android.permission.REQUEST_INSTALL_PACKAGES"));
        assertTrue(perms.contains("android.permission.FOREGROUND_SERVICE"));
        assertTrue(perms.contains("android.permission.FOREGROUND_SERVICE_DATA_SYNC"));
        assertTrue(perms.contains("android.permission.POST_NOTIFICATIONS"));

        ServiceInfo service = pm.getServiceInfo(new ComponentName(context, UpdateService.class), 0);
        assertFalse(service.exported);
        assertEquals(ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC, service.getForegroundServiceType());

        ActivityInfo receiver = pm.getReceiverInfo(new ComponentName(context, UpdateInstallReceiver.class), 0);
        assertFalse(receiver.exported);
    }

    @Test
    public void checkShortcutRunsACheckAndOpensTheHostList() {
        next = UpdateChecker.Result.upToDate("2026.11.19.1");
        Intent intent = new Intent(UpdateShortcuts.ACTION_CHECK).setClass(context, UpdateShortcutActivity.class);
        UpdateShortcutActivity activity = Robolectric.buildActivity(UpdateShortcutActivity.class, intent).create().get();

        assertTrue(activity.isFinishing());
        Intent started = Shadows.shadowOf(activity).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(PcView.class.getName(), started.getComponent().getClassName());

        // The host list then runs the check and says so
        openHostList();
        assertTrue("a manual check ran", fetches.get() >= 1);
        assertEquals(str(R.string.update_up_to_date), ShadowToast.getTextOfLatestToast());
    }

    @Test
    public void installShortcutShowsTheUpdateDialogNeverInstallsByItself() {
        // A check found the update earlier and stored it
        context.getSharedPreferences("update_check", Context.MODE_PRIVATE).edit()
                .putString("pending_update", installable().toJson()).putLong("last_success", System.currentTimeMillis()).commit();
        assertEquals(VERSION, UpdateNotifier.pendingVersion(context));

        Intent intent = new Intent(UpdateShortcuts.ACTION_INSTALL).setClass(context, UpdateShortcutActivity.class)
                .putExtra(UpdateShortcuts.EXTRA_VERSION, VERSION);
        Robolectric.buildActivity(UpdateShortcutActivity.class, intent).create();

        openHostList();
        AlertDialog dialog = latest();
        assertNotNull(dialog);
        assertEquals(str(R.string.update_available_title), title(dialog));
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertEquals(0, fetches.get());
    }

    @Test
    public void foreignOrMalformedShortcutIntentDoesNothing() {
        Intent intent = new Intent(Intent.ACTION_VIEW, android.net.Uri.parse("https://evil.example/")).setClass(context, UpdateShortcutActivity.class);
        UpdateShortcutActivity activity = Robolectric.buildActivity(UpdateShortcutActivity.class, intent).create().get();
        assertTrue(activity.isFinishing());
        assertNull(Shadows.shadowOf(activity).getNextStartedActivity());
    }

    @Test
    public void shortcutDuringAStreamNeverTouchesTheStream() {
        Game.instance = mock(Game.class);
        Intent intent = new Intent(UpdateShortcuts.ACTION_CHECK).setClass(context, UpdateShortcutActivity.class);
        UpdateShortcutActivity activity = Robolectric.buildActivity(UpdateShortcutActivity.class, intent).create().get();

        assertTrue(activity.isFinishing());
        assertNull(Shadows.shadowOf(activity).getNextStartedActivity());
        assertEquals(str(R.string.update_not_during_stream), ShadowToast.getTextOfLatestToast());
        assertEquals(0, fetches.get());
    }

    @Test
    public void dynamicShortcutsFollowThePendingUpdate() {
        openHostList();
        assertTrue(shortcutIds().contains(UpdateShortcuts.ID_CHECK));
        assertTrue(shortcutIds().contains(UpdateShortcuts.ID_INSTALL));

        // The app is now up to date: the install shortcut goes away
        UpdateShortcuts.refresh(context, null);
        assertTrue(shortcutIds().contains(UpdateShortcuts.ID_CHECK));
        assertFalse(shortcutIds().contains(UpdateShortcuts.ID_INSTALL));
    }

    private java.util.List<String> shortcutIds() {
        java.util.List<String> ids = new java.util.ArrayList<>();
        for (androidx.core.content.pm.ShortcutInfoCompat s : androidx.core.content.pm.ShortcutManagerCompat.getDynamicShortcuts(context)) {
            ids.add(s.getId());
        }
        return ids;
    }

    // ---- "Updated to", leftovers ----

    @Test
    public void updatedToIsShownOnceAfterTheRestart() {
        next = UpdateChecker.Result.upToDate(VERSION);
        UpdateInstaller.setTestHooks(null, null, null, VERSION + "-debug");
        context.getSharedPreferences("update_check", Context.MODE_PRIVATE).edit()
                .putString(UpdateInstaller.KEY_INSTALLING_VERSION, VERSION)
                .putLong(UpdateInstaller.KEY_INSTALLING_AT, System.currentTimeMillis() - 3000).commit();

        ActivityController<PcView> controller = openHostList();
        assertEquals(context.getString(R.string.update_done, VERSION), ShadowToast.getTextOfLatestToast());

        ShadowToast.reset();
        controller.pause().resume();
        assertNull(ShadowToast.getLatestToast());
    }

    @Test
    public void leftoverDownloadsAreRemovedOnStart() throws Exception {
        File dir = UpdateInstaller.updateDir(context);
        assertTrue(dir.isDirectory() || dir.mkdirs());
        File old = new File(dir, "update.apk");
        try (FileOutputStream out = new FileOutputStream(old)) {
            out.write(7);
        }
        openHostList();
        assertFalse(old.exists());
    }

    // ---- Foreground service ----

    @Test
    public void leavingTheAppDuringTheDownloadStartsTheServiceButRotationDoesNot() {
        ActivityController<PcView> controller = openHostList();
        click(latest(), AlertDialog.BUTTON_POSITIVE);
        assertFalse(startedUpdateService());

        controller.pause();
        assertTrue(startedUpdateService());
    }

    private boolean startedUpdateService() {
        Intent started;
        boolean found = false;
        while ((started = Shadows.shadowOf((android.app.Application) context).getNextStartedService()) != null) {
            found |= UpdateService.class.getName().equals(started.getComponent().getClassName());
        }
        return found;
    }

    @Test
    public void serviceShowsAnOngoingProgressNotificationWithCancel() {
        UpdateInstaller.start(context, installable());
        android.app.Service service = Robolectric.buildService(UpdateService.class).create().startCommand(0, 1).get();

        android.app.Notification n = Shadows.shadowOf(service).getLastForegroundNotification();
        assertNotNull(n);
        assertTrue((n.flags & android.app.Notification.FLAG_ONGOING_EVENT) != 0);
        assertNotNull(n.actions);
        assertEquals(1, n.actions.length);
        assertEquals(str(R.string.update_cancel), n.actions[0].title.toString());
    }

    @Test
    public void serviceStopsItselfWhenNothingIsRunning() {
        android.app.Service service = Robolectric.buildService(UpdateService.class).create().startCommand(0, 1).get();
        assertTrue(Shadows.shadowOf(service).isStoppedBySelf());
    }

    @Test
    public void serviceCancelActionCancelsTheDownload() {
        UpdateInstaller.start(context, installable());
        Intent cancel = new Intent(context, UpdateService.class).setAction(UpdateService.ACTION_CANCEL);
        Robolectric.buildService(UpdateService.class, cancel).create().startCommand(0, 1);
        // the worker has not started; once it does it sees the cancellation
        queuedInstall.run();
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
    }
}
