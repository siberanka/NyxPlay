package com.limelight.utils;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.lifecycle.DefaultLifecycleObserver;
import androidx.lifecycle.LifecycleOwner;

import com.limelight.R;

/**
 * Progress, cancel and failure screens of the update install for one visible activity. The work
 * itself runs in UpdateInstaller, so a new activity (rotation) simply attaches and shows the current state.
 */
final class UpdateProgressUi implements UpdateInstaller.Listener, DefaultLifecycleObserver {
    private static UpdateProgressUi current;

    private final AppCompatActivity activity;
    private AlertDialog dialog;
    private TextView text;
    private ProgressBar bar;
    private boolean started;
    private boolean detached;

    private UpdateProgressUi(AppCompatActivity activity) {
        this.activity = activity;
    }

    /** Shows the install progress in this activity from now on (idempotent per activity). */
    static synchronized void attach(AppCompatActivity activity) {
        if (activity.isFinishing() || activity.isDestroyed()) {
            return;
        }
        if (current != null && !current.detached) {
            if (current.activity == activity) {
                return;
            }
            current.detach();
        }
        UpdateProgressUi ui = new UpdateProgressUi(activity);
        current = ui;
        UpdateInstaller.addListener(ui);
        // Replays onCreate/onStart to catch up with an activity that is already running
        activity.getLifecycle().addObserver(ui);
    }

    private void detach() {
        detached = true;
        UpdateInstaller.removeListener(this);
        activity.getLifecycle().removeObserver(this);
        dismissDialog();
    }

    // ---- Lifecycle ----

    @Override
    public void onStart(@NonNull LifecycleOwner owner) {
        started = true;
        // Back in the app: the notification service is no longer needed
        UpdateService.stop(activity.getApplicationContext());
        onUpdateState(UpdateInstaller.getState());
    }

    @Override
    public void onPause(@NonNull LifecycleOwner owner) {
        UpdateInstaller.State state = UpdateInstaller.getState();
        boolean downloading = state.phase == UpdateInstaller.Phase.DOWNLOADING || state.phase == UpdateInstaller.Phase.VERIFYING;
        if (downloading && !activity.isChangingConfigurations()) {
            UpdateService.start(activity.getApplicationContext());
        }
    }

    @Override
    public void onStop(@NonNull LifecycleOwner owner) {
        started = false;
        dismissDialog();
    }

    @Override
    public void onDestroy(@NonNull LifecycleOwner owner) {
        detach();
    }

    // ---- State ----

    @Override
    public void onUpdateState(UpdateInstaller.State state) {
        if (!started || detached) {
            return;
        }
        switch (state.phase) {
            case IDLE:
                dismissDialog();
                break;
            case FAILED:
                showFailure(state);
                break;
            default:
                showProgress(state);
                if (state.phase == UpdateInstaller.Phase.INSTALLING && state.needsConfirmation) {
                    launchConfirmation();
                }
                break;
        }
    }

    private void showProgress(UpdateInstaller.State state) {
        if (dialog == null || !dialog.isShowing() || text == null) {
            dismissDialog();
            Context context = activity;
            LinearLayout layout = new LinearLayout(context);
            layout.setOrientation(LinearLayout.VERTICAL);
            int pad = (int) (20 * context.getResources().getDisplayMetrics().density);
            layout.setPadding(pad, pad / 2, pad, 0);
            text = new TextView(context);
            text.setTextSize(16);
            layout.addView(text);
            bar = new ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal);
            bar.setMax(100);
            layout.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

            dialog = new AlertDialog.Builder(activity)
                    .setTitle(activity.getString(R.string.update_installing_title, state.version))
                    .setView(layout)
                    .setCancelable(false)
                    .setNegativeButton(R.string.update_cancel, new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface d, int which) {
                            UpdateInstaller.cancel(activity);
                        }
                    })
                    .create();
            dialog.show();
        }

        if (state.phase == UpdateInstaller.Phase.DOWNLOADING && state.percent >= 0) {
            text.setText(activity.getString(R.string.update_state_downloading_percent, state.percent));
            bar.setIndeterminate(false);
            bar.setProgress(state.percent);
        } else {
            int res = state.phase == UpdateInstaller.Phase.DOWNLOADING ? R.string.update_state_downloading
                    : state.phase == UpdateInstaller.Phase.VERIFYING ? R.string.update_state_verifying
                    : state.needsConfirmation ? R.string.update_state_confirm : R.string.update_state_installing;
            text.setText(res);
            bar.setIndeterminate(true);
        }
    }

    private void launchConfirmation() {
        Intent confirm = UpdateInstaller.takeConfirmation();
        if (confirm == null) {
            return;
        }
        try {
            activity.startActivity(confirm);
        } catch (RuntimeException e) {
            // The system installer screen could not be opened
            UpdateInstaller.cancel(activity);
        }
    }

    private void showFailure(final UpdateInstaller.State state) {
        dismissDialog();
        final UpdateChecker.Result offer = UpdateNotifier.currentOffer();
        dialog = new AlertDialog.Builder(activity)
                .setTitle(R.string.update_failed_title)
                .setMessage(failureMessage(state.failure))
                .setPositiveButton(R.string.update_release_page, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        UpdateInstaller.acknowledge();
                        UpdateNotifier.openReleasePage(activity, offer);
                    }
                })
                .setNegativeButton(R.string.update_close, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        UpdateInstaller.acknowledge();
                    }
                })
                .setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override
                    public void onCancel(DialogInterface d) {
                        UpdateInstaller.acknowledge();
                    }
                })
                .create();
        dialog.show();
        text = null;
    }

    private String failureMessage(UpdateInstaller.Failure failure) {
        return activity.getString(failureMessageRes(failure));
    }

    static int failureMessageRes(UpdateInstaller.Failure failure) {
        if (failure == null) {
            return R.string.update_error_other;
        }
        switch (failure) {
            case NO_CONNECTION:
                return R.string.update_error_no_connection;
            case DOWNLOAD:
                return R.string.update_error_download;
            case NOT_VERIFIED:
                return R.string.update_error_not_verified;
            case STORAGE:
                return R.string.update_error_storage;
            case BLOCKED:
                return R.string.update_error_blocked;
            case CONFLICT:
                return R.string.update_error_conflict;
            case INCOMPATIBLE:
                return R.string.update_error_incompatible;
            case ABORTED:
                return R.string.update_error_aborted;
            case UNSUPPORTED:
                return R.string.update_error_unsupported;
            default:
                return R.string.update_error_other;
        }
    }

    private void dismissDialog() {
        if (dialog != null) {
            if (dialog.isShowing()) {
                dialog.dismiss();
            }
            dialog = null;
        }
        text = null;
        bar = null;
    }

    boolean isShowingForTest() {
        return dialog != null && dialog.isShowing();
    }

    static synchronized UpdateProgressUi currentForTest() {
        return current;
    }

    static synchronized void resetForTest() {
        if (current != null) {
            current.detached = true;
            UpdateInstaller.removeListener(current);
            current = null;
        }
    }
}
