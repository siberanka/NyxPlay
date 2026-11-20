package com.limelight.utils;

import android.Manifest;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
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
    private static final String KEY_PENDING = "pending_update";
    private static final String KEY_NOTIFICATION_ASKED = "notification_permission_asked";
    private static final int REQUEST_POST_NOTIFICATIONS = 7301;

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
    // The offer being installed, kept for the release page fallback of a failure
    private static UpdateChecker.Result installOffer;
    // The offer waiting for the user to allow installing from this app
    private static UpdateChecker.Result awaitingPermission;
    // A launcher shortcut request, handed over in-process by UpdateShortcutActivity
    private static UpdateShortcuts.Action pendingShortcut;
    private static boolean shortcutsPublished;
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
        Context appContext = activity.getApplicationContext();
        UpdateInstaller.cleanupLeftovers(appContext);
        UpdateProgressUi.attach(activity);
        announceUpdatedVersion(activity);
        if (!shortcutsPublished) {
            shortcutsPublished = true;
            UpdateShortcuts.refresh(appContext, pendingVersion(appContext));
        }
        if (resumeAfterPermission(activity) || handleShortcutAction(activity)) {
            return;
        }
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
            // The waiting update is what the "Install update" shortcut offers
            if (result.status == UpdateChecker.Status.UPDATE_AVAILABLE) {
                editor.putString(KEY_PENDING, result.toJson());
            } else {
                editor.remove(KEY_PENDING);
            }
        }
        editor.apply();
        running.set(false);
        if (result.status != UpdateChecker.Status.FAILED) {
            UpdateShortcuts.refresh(appContext, pendingVersion(appContext));
        }

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
        final boolean installable = notice.canInstallInApp();
        if (!installable && activity.getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK)) {
            // The built-in page viewer may be all a TV has, so the address is also shown to be typed elsewhere
            message.append("\n\n").append(activity.getString(R.string.update_download_address, notice.downloadUrl));
        }

        final AlertDialog[] holder = new AlertDialog[1];
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(R.string.update_available_title)
                .setMessage(message.toString())
                .setPositiveButton(installable ? R.string.update_install : R.string.update_download, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        pendingNotice = null;
                        if (installable) {
                            beginInstall(activity, notice);
                        } else {
                            openDownload(activity, notice);
                        }
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
                });
        if (installable) {
            // Secondary action next to the three buttons: the release page, for people who want to read or install by hand
            Button page = new Button(activity);
            page.setText(R.string.update_release_page);
            page.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    pendingNotice = null;
                    if (holder[0] != null) {
                        holder[0].dismiss();
                    }
                    openReleasePage(activity, notice);
                }
            });
            LinearLayout box = new LinearLayout(activity);
            box.setOrientation(LinearLayout.VERTICAL);
            int pad = (int) (16 * activity.getResources().getDisplayMetrics().density);
            box.setPadding(pad, 0, pad, 0);
            box.addView(page);
            builder.setView(box);
        }
        final AlertDialog dialog = builder.create();
        holder[0] = dialog;

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

    static void openReleasePage(Context context, UpdateChecker.Result offer) {
        String url = offer != null && UpdateChecker.isAllowedUrl(offer.pageUrl) ? offer.pageUrl : UpdateChecker.RELEASES_PAGE_URL;
        try {
            HelpLauncher.launchUrl(context, url);
        } catch (Exception e) {
            // No browser and no page viewer
        }
    }

    static UpdateChecker.Result currentOffer() {
        return installOffer;
    }

    // ---- Install flow ----

    /** The Install button: checks the permission to install, explains it if needed, then starts the download. */
    static void beginInstall(final AppCompatActivity activity, final UpdateChecker.Result notice) {
        installOffer = notice;
        UpdateAccess.Access access = UpdateAccess.check(activity);
        switch (access) {
            case ALLOWED:
                startInstall(activity, notice);
                break;
            case NEEDS_APP_SETTING:
            case NEEDS_GLOBAL_SETTING:
                explainPermission(activity, notice, access);
                break;
            default:
                // Cannot be allowed from here (for example a TV without that settings screen)
                Toast.makeText(activity, R.string.update_install_unavailable, Toast.LENGTH_LONG).show();
                openReleasePage(activity, notice);
                break;
        }
    }

    private static void explainPermission(final AppCompatActivity activity, final UpdateChecker.Result notice,
                                          final UpdateAccess.Access access) {
        int message = access == UpdateAccess.Access.NEEDS_APP_SETTING
                ? R.string.update_permission_message : R.string.update_permission_message_legacy;
        new AlertDialog.Builder(activity)
                .setTitle(R.string.update_permission_title)
                .setMessage(message)
                .setPositiveButton(R.string.update_open_settings, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        Intent settings = UpdateAccess.settingsIntent(activity, access);
                        try {
                            // The offer continues when the user is back on the host list
                            awaitingPermission = notice;
                            activity.startActivity(settings);
                        } catch (RuntimeException e) {
                            awaitingPermission = null;
                            Toast.makeText(activity, R.string.update_install_unavailable, Toast.LENGTH_LONG).show();
                            openReleasePage(activity, notice);
                        }
                    }
                })
                .setNeutralButton(R.string.update_release_page, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        openReleasePage(activity, notice);
                    }
                })
                .setNegativeButton(R.string.update_cancel, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        // The update stays offered the next time the host list shows
                        pendingNotice = notice;
                    }
                })
                .show();
    }

    private static boolean resumeAfterPermission(AppCompatActivity activity) {
        UpdateChecker.Result offer = awaitingPermission;
        if (offer == null) {
            return false;
        }
        awaitingPermission = null;
        if (UpdateAccess.check(activity) == UpdateAccess.Access.ALLOWED) {
            startInstall(activity, offer);
            return true;
        }
        // Still not allowed: offer the update again instead of failing silently
        pendingNotice = offer;
        return false;
    }

    private static void startInstall(AppCompatActivity activity, UpdateChecker.Result notice) {
        installOffer = notice;
        UpdateProgressUi.attach(activity);
        if (UpdateInstaller.start(activity.getApplicationContext(), notice)) {
            askForNotificationsOnce(activity);
        }
    }

    // The progress notification is optional, so this is asked once and the answer is not waited for
    private static void askForNotificationsOnce(AppCompatActivity activity) {
        if (Build.VERSION.SDK_INT < 33
                || ContextCompat.checkSelfPermission(activity, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        SharedPreferences prefs = state(activity);
        if (prefs.getBoolean(KEY_NOTIFICATION_ASKED, false)) {
            return;
        }
        prefs.edit().putBoolean(KEY_NOTIFICATION_ASKED, true).apply();
        try {
            ActivityCompat.requestPermissions(activity, new String[] {Manifest.permission.POST_NOTIFICATIONS}, REQUEST_POST_NOTIFICATIONS);
        } catch (RuntimeException ignored) {
            // The install does not depend on it
        }
    }

    // ---- Shortcuts, pending offer and "updated" notice ----

    /** The launcher shortcut request; consumed by the next visible host list. */
    public static void requestShortcutAction(UpdateShortcuts.Action action) {
        pendingShortcut = action;
    }

    private static boolean handleShortcutAction(AppCompatActivity activity) {
        UpdateShortcuts.Action action = pendingShortcut;
        pendingShortcut = null;
        if (action == null || action == UpdateShortcuts.Action.NONE) {
            return false;
        }
        Context appContext = activity.getApplicationContext();
        if (action == UpdateShortcuts.Action.INSTALL && pendingVersion(appContext) != null) {
            UpdateChecker.Result offer = loadOffer(appContext);
            if (offer != null) {
                // The install still goes through the dialog, so a launcher tap cannot install by itself
                pendingNotice = offer;
                return showPendingNotice(activity);
            }
        }
        checkNow(activity);
        return true;
    }

    private static UpdateChecker.Result loadOffer(Context context) {
        String json = state(context).getString(KEY_PENDING, "");
        return json.isEmpty() ? null : UpdateChecker.Result.fromJson(json);
    }

    /** Version of the update that is waiting and newer than the installed app, else null (and the record is dropped). */
    public static String pendingVersion(Context context) {
        UpdateChecker.Result offer = loadOffer(context);
        if (offer != null && UpdateInstaller.isNewerThanInstalled(offer.version)) {
            return offer.version;
        }
        if (offer != null || !state(context).getString(KEY_PENDING, "").isEmpty()) {
            state(context).edit().remove(KEY_PENDING).apply();
        }
        return null;
    }

    private static void announceUpdatedVersion(AppCompatActivity activity) {
        Context appContext = activity.getApplicationContext();
        String version = UpdateInstaller.consumeUpdatedVersion(appContext);
        if (version == null) {
            return;
        }
        state(appContext).edit().remove(KEY_PENDING).apply();
        UpdateShortcuts.refresh(appContext, null);
        Toast.makeText(activity, activity.getString(R.string.update_done, version), Toast.LENGTH_LONG).show();
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
        installOffer = null;
        awaitingPermission = null;
        pendingShortcut = null;
        shortcutsPublished = false;
        UpdateProgressUi.resetForTest();
        mainScreen = new WeakReference<>(null);
        running.set(false);
    }
}
