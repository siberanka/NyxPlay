package com.limelight.utils;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;

/** Receives the PackageInstaller result of an update install. Not exported; only our PendingIntent targets it. */
public class UpdateInstallReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ApkInstaller.ACTION_STATUS.equals(intent.getAction())) {
            return;
        }
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        int sessionId = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1);
        Intent confirmation = null;
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            confirmation = confirmationFrom(intent);
        }
        UpdateInstaller.onInstallStatus(context.getApplicationContext(), status, sessionId, confirmation);
    }

    @SuppressWarnings("deprecation")
    private static Intent confirmationFrom(Intent intent) {
        Intent confirmation = intent.getParcelableExtra(Intent.EXTRA_INTENT);
        if (confirmation == null) {
            return null;
        }
        // Only the system's installer screen is expected; it is opened as its own task
        confirmation.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return confirmation;
    }
}
