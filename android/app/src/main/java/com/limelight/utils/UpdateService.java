package com.limelight.utils;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.app.ServiceCompat;
import androidx.core.content.ContextCompat;

import com.limelight.R;

/**
 * Keeps an update download alive, with an ongoing progress notification, while the user is outside
 * the app. It is started only when the app is left during a download and stops as soon as the app is
 * visible again or the run ends.
 */
public class UpdateService extends Service implements UpdateInstaller.Listener {
    static final String ACTION_CANCEL = "com.limelight.action.UPDATE_CANCEL";
    static final String CHANNEL_ID = "nyxplay_update";
    static final int NOTIFICATION_ID = 7301;
    static final int RESULT_NOTIFICATION_ID = 7302;

    private boolean listening;

    /** Best effort: the system may refuse a foreground start from the background. */
    public static void start(Context context) {
        try {
            ContextCompat.startForegroundService(context, new Intent(context, UpdateService.class));
        } catch (RuntimeException e) {
            // Not allowed right now; the download simply continues while the process lives
        }
    }

    public static void stop(Context context) {
        try {
            context.stopService(new Intent(context, UpdateService.class));
        } catch (RuntimeException ignored) {
            // Nothing to stop
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Must enter the foreground at once; the notification matches the current state
        UpdateInstaller.State state = UpdateInstaller.getState();
        showForeground(state);

        if (intent != null && ACTION_CANCEL.equals(intent.getAction())) {
            UpdateInstaller.cancel(this);
        }
        if (!listening) {
            listening = true;
            UpdateInstaller.addListener(this);
        }
        if (!UpdateInstaller.getState().isActive()) {
            finishSelf(UpdateInstaller.getState());
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        UpdateInstaller.removeListener(this);
        listening = false;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onUpdateState(UpdateInstaller.State state) {
        if (!state.isActive() || state.needsConfirmation) {
            finishSelf(state);
            return;
        }
        NotificationManagerCompat.from(this).notify(NOTIFICATION_ID, build(state));
    }

    private void showForeground(UpdateInstaller.State state) {
        Notification notification = build(state);
        if (Build.VERSION.SDK_INT >= 29) {
            ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    /** Leaves a plain notification when the user still has something to do or to read, then stops. */
    private void finishSelf(UpdateInstaller.State state) {
        stopForeground(true);
        NotificationManagerCompat manager = NotificationManagerCompat.from(this);
        if (state.needsConfirmation) {
            manager.notify(RESULT_NOTIFICATION_ID, resultNotification(getString(R.string.update_notification_ready)));
        } else if (state.phase == UpdateInstaller.Phase.FAILED) {
            manager.notify(RESULT_NOTIFICATION_ID, resultNotification(getString(R.string.update_failed_title)));
        }
        stopSelf();
    }

    private Notification resultNotification(String text) {
        ensureChannel();
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification_update)
                .setContentTitle(getString(R.string.update_notification_title))
                .setContentText(text)
                .setAutoCancel(true)
                .setContentIntent(openAppIntent())
                .build();
    }

    private Notification build(UpdateInstaller.State state) {
        ensureChannel();
        int textRes = state.phase == UpdateInstaller.Phase.DOWNLOADING ? R.string.update_state_downloading
                : state.phase == UpdateInstaller.Phase.VERIFYING ? R.string.update_state_verifying
                : R.string.update_state_installing;
        String text = getString(textRes);
        boolean determinate = state.phase == UpdateInstaller.Phase.DOWNLOADING && state.percent >= 0;
        if (determinate) {
            text = getString(R.string.update_state_downloading_percent, state.percent);
        }

        Intent cancel = new Intent(this, UpdateService.class).setAction(ACTION_CANCEL);
        PendingIntent cancelIntent = PendingIntent.getService(this, 0, cancel,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification_update)
                .setContentTitle(getString(R.string.update_notification_title))
                .setContentText(text)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setProgress(100, Math.max(0, state.percent), !determinate)
                .setContentIntent(openAppIntent())
                .addAction(0, getString(R.string.update_cancel), cancelIntent)
                .build();
    }

    private PendingIntent openAppIntent() {
        // The normal launcher entry, like tapping the icon; never clears the stack, so a stream is untouched
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch == null) {
            launch = new Intent();
        }
        return PendingIntent.getActivity(this, 0, launch, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
                nm.createNotificationChannel(new NotificationChannel(CHANNEL_ID,
                        getString(R.string.update_notification_channel), NotificationManager.IMPORTANCE_LOW));
            }
        }
    }
}
