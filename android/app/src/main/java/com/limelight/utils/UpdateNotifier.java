package com.limelight.utils;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.lifecycle.DefaultLifecycleObserver;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;
import androidx.preference.PreferenceManager;

import com.limelight.BuildConfig;
import com.limelight.R;

import java.lang.ref.WeakReference;
import java.util.Arrays;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Glue between the settings, the host list screen and UpdateChecker: decides when an automatic
 * check is due, runs it off the main thread, and shows the result only to a live, visible activity.
 */
public final class UpdateNotifier {
    public static final String PREF_AUTO_CHECK = "checkbox_check_updates";

    private static final String STATE_FILE = "update_check";
    private static final String KEY_LAST_SUCCESS = "last_success";
    private static final String KEY_LAST_FAILURE = "last_failure";
    private static final String KEY_SKIPPED = "skipped_version";

    static final long SUCCESS_INTERVAL_MS = 24L * 60 * 60 * 1000;
    // A failed automatic check is retried sooner so a temporary outage does not hide an update for a day
    static final long FAILURE_RETRY_MS = 60L * 60 * 1000;

    /** Produces a check result; replaced in tests. */
    public interface Fetcher {
        UpdateChecker.Result fetch();
    }

    private static final AtomicBoolean running = new AtomicBoolean(false);
    private static ExecutorService defaultExecutor;
    private static Executor executor;
    private static Fetcher fetcher;

    // The notice the user has not answered yet; survives the activity being recreated
    private static UpdateChecker.Result pendingNotice;
    private static AlertDialog shownDialog;
    // The host list that was last visible; a check started before a rotation reports to its successor
    private static WeakReference<AppCompatActivity> mainScreen = new WeakReference<>(null);

    private UpdateNotifier() {}

    // ---- Entry points ----

    /** Called when the host list becomes visible. Shows an unanswered notice, else runs a check if one is due. */
    public static void onMainScreenVisible(final AppCompatActivity activity) {
        if (activity.isFinishing() || activity.isDestroyed()) {
            return;
        }
        mainScreen = new WeakReference<>(activity);
        if (showPendingNotice(activity)) {
            return;
        }
        if (!isAutoCheckEnabled(activity) || !isCheckDue(activity, System.currentTimeMillis())) {
            return;
        }
        startCheck(activity, false);
    }

    /** Always checks and always reports the outcome. */
    public static void checkNow(final AppCompatActivity activity) {
        if (!running.get()) {
            Toast.makeText(activity, R.string.update_checking, Toast.LENGTH_SHORT).show();
        }
        startCheck(activity, true);
    }

    public static boolean isAutoCheckEnabled(Context context) {
        try {
            return PreferenceManager.getDefaultSharedPreferences(context).getBoolean(PREF_AUTO_CHECK, true);
        } catch (Exception e) {
            return true;
        }
    }

    // ---- Throttle and skip state ----

    static boolean isCheckDue(Context context, long now) {
        SharedPreferences state = state(context);
        return isDue(now, state.getLong(KEY_LAST_SUCCESS, 0), state.getLong(KEY_LAST_FAILURE, 0));
    }

    static boolean isDue(long now, long lastSuccess, long lastFailure) {
        // A timestamp in the future (clock change) must not block checks for good
        boolean successOld = lastSuccess <= 0 || lastSuccess > now || now - lastSuccess >= SUCCESS_INTERVAL_MS;
        boolean failureOld = lastFailure <= 0 || lastFailure > now || now - lastFailure >= FAILURE_RETRY_MS;
        return successOld && failureOld;
    }

    static String getSkippedVersion(Context context) {
        return state(context).getString(KEY_SKIPPED, "");
    }

    static void skipVersion(Context context, String version) {
        state(context).edit().putString(KEY_SKIPPED, version).apply();
    }

    private static SharedPreferences state(Context context) {
        return context.getApplicationContext().getSharedPreferences(STATE_FILE, Context.MODE_PRIVATE);
    }

    // ---- Checking ----

    private static void startCheck(AppCompatActivity activity, final boolean manual) {
        if (!running.compareAndSet(false, true)) {
            return;
        }

        final Context appContext = activity.getApplicationContext();
        final WeakReference<AppCompatActivity> ref = new WeakReference<>(activity);
        try {
            executor().execute(new Runnable() {
                @Override
                public void run() {
                    UpdateChecker.Result result;
                    try {
                        result = currentFetcher().fetch();
                    } catch (Throwable t) {
                        result = UpdateChecker.Result.failed(UpdateChecker.Failure.BAD_RESPONSE);
                    }
                    finish(appContext, ref, result, manual);
                }
            });
        } catch (RuntimeException e) {
            running.set(false);
        }
    }

    private static void finish(final Context appContext, final WeakReference<AppCompatActivity> ref,
                               final UpdateChecker.Result result, final boolean manual) {
        // Only the completion of a check moves the throttle; a manual check also counts
        SharedPreferences.Editor editor = state(appContext).edit();
        if (result.status == UpdateChecker.Status.FAILED) {
            editor.putLong(KEY_LAST_FAILURE, System.currentTimeMillis());
        } else {
            editor.putLong(KEY_LAST_SUCCESS, System.currentTimeMillis()).putLong(KEY_LAST_FAILURE, 0);
        }
        editor.apply();
        running.set(false);

        AppCompatActivity activity = ref.get();
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            activity = manual ? null : mainScreen.get();
        }
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            // Nobody to tell right now; an update is kept for the next time the host list shows
            if (!manual) {
                rememberIfOffered(appContext, result);
            }
            return;
        }

        final AppCompatActivity target = activity;
        target.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                deliver(target, appContext, result, manual);
            }
        });
    }

    private static void deliver(AppCompatActivity activity, Context appContext, UpdateChecker.Result result, boolean manual) {
        if (activity.isFinishing() || activity.isDestroyed()) {
            if (!manual) {
                rememberIfOffered(appContext, result);
            }
            return;
        }

        switch (result.status) {
            case UPDATE_AVAILABLE:
                if (!manual && result.version.equals(getSkippedVersion(appContext))) {
                    return;
                }
                pendingNotice = result;
                showPendingNotice(activity);
                break;

            case UP_TO_DATE:
                if (manual) {
                    Toast.makeText(activity, R.string.update_up_to_date, Toast.LENGTH_LONG).show();
                }
                break;

            default:
                if (manual) {
                    int message = result.failure == UpdateChecker.Failure.NO_CONNECTION
                            ? R.string.update_check_failed_offline : R.string.update_check_failed_response;
                    Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
                }
                break;
        }
    }

    private static void rememberIfOffered(Context appContext, UpdateChecker.Result result) {
        if (result.status == UpdateChecker.Status.UPDATE_AVAILABLE
                && !result.version.equals(getSkippedVersion(appContext))) {
            pendingNotice = result;
        }
    }

    // ---- Dialog ----

    /** Shows the unanswered notice if the activity is visible. Returns true when a notice is pending. */
    static boolean showPendingNotice(final AppCompatActivity activity) {
        final UpdateChecker.Result notice = pendingNotice;
        if (notice == null) {
            return false;
        }
        if (activity.isFinishing() || activity.isDestroyed()
                || !activity.getLifecycle().getCurrentState().isAtLeast(Lifecycle.State.STARTED)) {
            return true;
        }
        if (shownDialog != null && shownDialog.isShowing()) {
            return true;
        }

        final Context appContext = activity.getApplicationContext();
        StringBuilder message = new StringBuilder(activity.getString(R.string.update_available_message, notice.version));
        if (notice.notes != null && !notice.notes.isEmpty()) {
            message.append("\n\n").append(notice.notes);
        }
        if (activity.getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK)) {
            // The built-in page viewer may be all a TV has, so the address is also shown to be typed elsewhere
            message.append("\n\n").append(activity.getString(R.string.update_download_address, notice.downloadUrl));
        }

        final AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle(R.string.update_available_title)
                .setMessage(message.toString())
                .setPositiveButton(R.string.update_download, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        pendingNotice = null;
                        openDownload(activity, notice);
                    }
                })
                .setNeutralButton(R.string.update_later, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        pendingNotice = null;
                    }
                })
                .setNegativeButton(R.string.update_skip_version, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        pendingNotice = null;
                        skipVersion(appContext, notice.version);
                    }
                })
                .setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override
                    public void onCancel(DialogInterface d) {
                        // Back button: same as Later
                        pendingNotice = null;
                    }
                })
                .create();

        // The window must not outlive the activity (rotation, finish); the notice itself stays pending
        activity.getLifecycle().addObserver(new DefaultLifecycleObserver() {
            @Override
            public void onDestroy(@NonNull LifecycleOwner owner) {
                if (dialog.isShowing()) {
                    dialog.dismiss();
                }
                owner.getLifecycle().removeObserver(this);
            }
        });

        shownDialog = dialog;
        dialog.show();
        return true;
    }

    private static void openDownload(Context context, UpdateChecker.Result notice) {
        String url = UpdateChecker.isAllowedUrl(notice.downloadUrl) ? notice.downloadUrl : UpdateChecker.RELEASES_PAGE_URL;
        try {
            HelpLauncher.launchUrl(context, url);
        } catch (Exception e) {
            // No browser and no page viewer: the address is already visible on leanback devices
        }
    }

    // ---- Wiring ----

    private static synchronized Executor executor() {
        if (executor != null) {
            return executor;
        }
        if (defaultExecutor == null) {
            defaultExecutor = Executors.newSingleThreadExecutor(new java.util.concurrent.ThreadFactory() {
                @Override
                public Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "NyxPlay-UpdateCheck");
                    t.setDaemon(true);
                    return t;
                }
            });
        }
        return defaultExecutor;
    }

    private static Fetcher currentFetcher() {
        Fetcher f = fetcher;
        if (f != null) {
            return f;
        }
        return new Fetcher() {
            @Override
            public UpdateChecker.Result fetch() {
                return new UpdateChecker(BuildConfig.VERSION_NAME, Arrays.asList(Build.SUPPORTED_ABIS)).check();
            }
        };
    }

    /** Test hook: replaces the network fetch and the thread it runs on (null restores the defaults). */
    public static synchronized void setTestHooks(Fetcher testFetcher, Executor testExecutor) {
        fetcher = testFetcher;
        executor = testExecutor;
        pendingNotice = null;
        shownDialog = null;
        mainScreen = new WeakReference<>(null);
        running.set(false);
    }
}
