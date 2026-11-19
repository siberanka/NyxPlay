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
import android.content.SharedPreferences;
import android.os.Build;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.PcView;
import com.limelight.R;
import com.limelight.TestLogSuppressor;
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

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class UpdateNotifierTest {
    private static final String ARM64_URL =
            "https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/NyxPlay-2026.12.1.1-arm64-v8a-release.apk";
    private static final String PAGE_URL = "https://github.com/siberanka/NyxPlay/releases/tag/v2026.12.1.1";

    private final AtomicInteger fetches = new AtomicInteger();
    private volatile UpdateChecker.Result next;
    private Context context;

    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    @Before
    public void setUp() {
        context = ApplicationProvider.getApplicationContext();

        // Same environment the other PcView tests use
        context.getSharedPreferences("GlPreferences", 0).edit()
                .putString("Renderer", "TestRenderer")
                .putString("Fingerprint", Build.FINGERPRINT)
                .commit();
        org.robolectric.shadows.ShadowApplication shadowApp =
                Shadows.shadowOf((android.app.Application) context);
        shadowApp.setSystemService(Context.GAME_SERVICE, mock(GameManager.class));
        ComputerManagerService.ComputerManagerBinder binder = mock(ComputerManagerService.ComputerManagerBinder.class);
        shadowApp.setComponentNameAndServiceForBindService(new ComponentName(context, ComputerManagerService.class), binder);

        next = UpdateChecker.Result.update("2026.12.1.1", "Fixes and improvements.", ARM64_URL, PAGE_URL);
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
    }

    @After
    public void tearDown() {
        UpdateNotifier.setTestHooks(null, null);
    }

    private ActivityController<PcView> openHostList() {
        return Robolectric.buildActivity(PcView.class).setup();
    }

    private AlertDialog updateDialog() {
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog == null || !dialog.isShowing()) {
            return null;
        }
        String title = String.valueOf(Shadows.shadowOf(dialog).getTitle());
        return title.equals(context.getString(R.string.update_available_title)) ? dialog : null;
    }

    private static void click(AlertDialog dialog, int which) {
        dialog.getButton(which).performClick();
        // The dialog dismisses itself through a handler message
        Shadows.shadowOf(android.os.Looper.getMainLooper()).idle();
    }

    private SharedPreferences state() {
        return context.getSharedPreferences("update_check", Context.MODE_PRIVATE);
    }

    // ---- Throttle and skip logic ----

    @Test
    public void throttleAllowsFirstCheckAndBlocksWithin24Hours() {
        long now = 1_800_000_000_000L;
        assertTrue(UpdateNotifier.isDue(now, 0, 0));
        assertFalse(UpdateNotifier.isDue(now, now - 1000, 0));
        assertFalse(UpdateNotifier.isDue(now, now - UpdateNotifier.SUCCESS_INTERVAL_MS + 1, 0));
        assertTrue(UpdateNotifier.isDue(now, now - UpdateNotifier.SUCCESS_INTERVAL_MS, 0));
    }

    @Test
    public void failedCheckIsRetriedAfterAnHourNotAtOnce() {
        long now = 1_800_000_000_000L;
        long lastSuccess = now - 3 * UpdateNotifier.SUCCESS_INTERVAL_MS;
        assertFalse(UpdateNotifier.isDue(now, lastSuccess, now - 1000));
        assertTrue(UpdateNotifier.isDue(now, lastSuccess, now - UpdateNotifier.FAILURE_RETRY_MS));
    }

    @Test
    public void timestampsFromTheFutureDoNotBlockChecks() {
        long now = 1_800_000_000_000L;
        assertTrue(UpdateNotifier.isDue(now, now + 5 * UpdateNotifier.SUCCESS_INTERVAL_MS, now + 1000));
    }

    @Test
    public void skippedVersionIsStoredPerVersion() {
        assertEquals("", UpdateNotifier.getSkippedVersion(context));
        UpdateNotifier.skipVersion(context, "2026.12.1.1");
        assertEquals("2026.12.1.1", UpdateNotifier.getSkippedVersion(context));
    }

    // ---- Host list behaviour ----

    @Test
    public void updateAvailableShowsDialogWithThreeActions() {
        openHostList();

        AlertDialog dialog = updateDialog();
        assertNotNull("update dialog expected", dialog);
        assertEquals(1, fetches.get());
        assertNotNull(dialog.getButton(AlertDialog.BUTTON_POSITIVE));
        assertNotNull(dialog.getButton(AlertDialog.BUTTON_NEUTRAL));
        assertNotNull(dialog.getButton(AlertDialog.BUTTON_NEGATIVE));
        assertEquals(context.getString(R.string.update_download), dialog.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertEquals(context.getString(R.string.update_later), dialog.getButton(AlertDialog.BUTTON_NEUTRAL).getText().toString());
        assertEquals(context.getString(R.string.update_skip_version), dialog.getButton(AlertDialog.BUTTON_NEGATIVE).getText().toString());
        assertTrue(Shadows.shadowOf(dialog).getMessage().toString().contains("2026.12.1.1"));
        assertTrue(Shadows.shadowOf(dialog).getMessage().toString().contains("Fixes and improvements."));
        assertTrue(state().getLong("last_success", 0) > 0);
    }

    @Test
    public void upToDateShowsNothing() {
        next = UpdateChecker.Result.upToDate("2026.11.19.1");
        openHostList();
        assertEquals(1, fetches.get());
        assertNull(updateDialog());
    }

    @Test
    public void failureShowsNothingAutomatically() {
        next = UpdateChecker.Result.failed(UpdateChecker.Failure.NO_CONNECTION);
        openHostList();
        assertEquals(1, fetches.get());
        assertNull(updateDialog());
        assertEquals(0, state().getLong("last_success", 0));
        assertTrue(state().getLong("last_failure", 0) > 0);
    }

    @Test
    public void automaticCheckHonoursTheSwitch() {
        context.getSharedPreferences(context.getPackageName() + "_preferences", Context.MODE_PRIVATE)
                .edit().putBoolean(UpdateNotifier.PREF_AUTO_CHECK, false).commit();
        openHostList();
        assertEquals(0, fetches.get());
        assertNull(updateDialog());
    }

    @Test
    public void secondVisitWithinADayDoesNotCheckAgain() {
        next = UpdateChecker.Result.upToDate("2026.11.19.1");
        ActivityController<PcView> controller = openHostList();
        assertEquals(1, fetches.get());

        controller.pause().resume();
        controller.pause().resume();
        assertEquals(1, fetches.get());

        // A day later the next visit checks again
        state().edit().putLong("last_success", System.currentTimeMillis() - UpdateNotifier.SUCCESS_INTERVAL_MS - 1000).commit();
        controller.pause().resume();
        assertEquals(2, fetches.get());
    }

    @Test
    public void laterClosesTheDialogAndDoesNotNagAtOnce() {
        ActivityController<PcView> controller = openHostList();
        click(updateDialog(), AlertDialog.BUTTON_NEUTRAL);
        assertNull(updateDialog());
        assertEquals("", UpdateNotifier.getSkippedVersion(context));

        controller.pause().resume();
        assertNull(updateDialog());
        assertEquals(1, fetches.get());

        // The next due automatic check offers it again
        state().edit().putLong("last_success", 1).commit();
        controller.pause().resume();
        assertNotNull(updateDialog());
    }

    @Test
    public void skipThisVersionIsRememberedAndNewerVersionIsOfferedAgain() {
        ActivityController<PcView> controller = openHostList();
        click(updateDialog(), AlertDialog.BUTTON_NEGATIVE);
        assertEquals("2026.12.1.1", UpdateNotifier.getSkippedVersion(context));
        assertNull(updateDialog());

        // Same version, due again: stays silent
        state().edit().putLong("last_success", 1).commit();
        controller.pause().resume();
        assertEquals(2, fetches.get());
        assertNull(updateDialog());

        // A newer version is offered
        next = UpdateChecker.Result.update("2026.12.2.1", "", ARM64_URL, PAGE_URL);
        state().edit().putLong("last_success", 1).commit();
        controller.pause().resume();
        assertNotNull(updateDialog());
    }

    @Test
    public void downloadOpensTheMatchingApk() {
        ActivityController<PcView> controller = openHostList();
        click(updateDialog(), AlertDialog.BUTTON_POSITIVE);

        Intent started = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(Intent.ACTION_VIEW, started.getAction());
        assertEquals(ARM64_URL, started.getData().toString());
        assertNull(updateDialog());
    }

    @Test
    public void downloadOfAForeignUrlOpensTheFixedReleasePage() {
        next = UpdateChecker.Result.update("2026.12.1.1", "", "https://evil.example/a.apk", "https://evil.example/");
        ActivityController<PcView> controller = openHostList();
        click(updateDialog(), AlertDialog.BUTTON_POSITIVE);

        Intent started = Shadows.shadowOf(controller.get()).getNextStartedActivity();
        assertEquals(UpdateChecker.RELEASES_PAGE_URL, started.getData().toString());
    }

    @Test
    public void destroyingTheActivityClosesTheDialogAndTheNoticeReturnsAfterRecreation() {
        ActivityController<PcView> first = openHostList();
        AlertDialog shown = updateDialog();
        assertNotNull(shown);

        // Rotation: the old activity goes away with the dialog open
        first.pause().stop().destroy();
        assertFalse("dialog window must not outlive the activity", shown.isShowing());

        // The new instance shows the still unanswered notice without another network request
        openHostList();
        assertNotNull(updateDialog());
        assertEquals(1, fetches.get());
    }

    @Test
    public void resultArrivingAfterTheActivityIsGoneIsNotDeliveredToIt() {
        final Runnable[] queued = new Runnable[1];
        UpdateNotifier.setTestHooks(new UpdateNotifier.Fetcher() {
            @Override
            public UpdateChecker.Result fetch() {
                fetches.incrementAndGet();
                return next;
            }
        }, new Executor() {
            @Override
            public void execute(Runnable command) {
                queued[0] = command;
            }
        });

        ActivityController<PcView> controller = openHostList();
        assertNotNull(queued[0]);
        assertNull(updateDialog());

        controller.pause().stop().destroy();
        queued[0].run(); // the background check finishes late: must not crash or leak a window
        assertNull(updateDialog());

        // The notice is kept for the next host list
        openHostList();
        assertNotNull(updateDialog());
    }

    @Test
    public void noticeIsNotShownWhileTheHostListIsNotVisible() {
        final Runnable[] queued = new Runnable[1];
        UpdateNotifier.setTestHooks(new UpdateNotifier.Fetcher() {
            @Override
            public UpdateChecker.Result fetch() {
                return next;
            }
        }, new Executor() {
            @Override
            public void execute(Runnable command) {
                queued[0] = command;
            }
        });

        ActivityController<PcView> controller = openHostList();
        // The user starts a stream (host list stops) before the check completes
        controller.pause().stop();
        queued[0].run();
        assertNull(updateDialog());

        controller.restart().start().resume();
        assertNotNull(updateDialog());
    }

    // ---- Manual check ----

    @Test
    public void manualCheckReportsUpToDate() {
        next = UpdateChecker.Result.upToDate("2026.11.19.1");
        ActivityController<PcView> controller = openHostList();
        // Throttled automatic check is already done; manual must still run
        UpdateNotifier.checkNow(controller.get());
        assertEquals(2, fetches.get());
        assertEquals(context.getString(R.string.update_up_to_date), ShadowToast.getTextOfLatestToast());
    }

    @Test
    public void manualCheckReportsFailureReason() {
        next = UpdateChecker.Result.upToDate("2026.11.19.1");
        ActivityController<PcView> controller = openHostList();

        next = UpdateChecker.Result.failed(UpdateChecker.Failure.NO_CONNECTION);
        UpdateNotifier.checkNow(controller.get());
        assertEquals(context.getString(R.string.update_check_failed_offline), ShadowToast.getTextOfLatestToast());

        next = UpdateChecker.Result.failed(UpdateChecker.Failure.BAD_RESPONSE);
        UpdateNotifier.checkNow(controller.get());
        assertEquals(context.getString(R.string.update_check_failed_response), ShadowToast.getTextOfLatestToast());
    }

    @Test
    public void manualCheckOffersAPreviouslySkippedVersion() {
        UpdateNotifier.skipVersion(context, "2026.12.1.1");
        ActivityController<PcView> controller = openHostList();
        assertNull("skipped version stays silent automatically", updateDialog());

        UpdateNotifier.checkNow(controller.get());
        assertNotNull(updateDialog());
    }

    @Test
    public void fetcherCrashBecomesAFailedCheck() {
        UpdateNotifier.setTestHooks(new UpdateNotifier.Fetcher() {
            @Override
            public UpdateChecker.Result fetch() {
                throw new IllegalStateException("boom");
            }
        }, new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        });
        ActivityController<PcView> controller = openHostList();
        assertNull(updateDialog());
        UpdateNotifier.checkNow(controller.get());
        assertEquals(context.getString(R.string.update_check_failed_response), ShadowToast.getTextOfLatestToast());
    }
}
