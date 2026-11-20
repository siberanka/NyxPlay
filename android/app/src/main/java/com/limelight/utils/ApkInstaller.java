package com.limelight.utils;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Installs a verified APK through a PackageInstaller session. The session is restricted to this
 * app's own package name, and the platform still requires the same signing certificate.
 */
final class ApkInstaller implements UpdateInstaller.Backend {
    static final String ACTION_STATUS = "com.limelight.action.UPDATE_INSTALL_STATUS";

    @Override
    public int install(Context context, File apk) throws IOException {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(context.getPackageName());
        params.setSize(apk.length());

        int id = installer.createSession(params);
        UpdateInstaller.sessionCreated(id);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("update.apk", 0, apk.length())) {
                byte[] buffer = new byte[64 * 1024];
                int n;
                while ((n = in.read(buffer)) >= 0) {
                    out.write(buffer, 0, n);
                }
                session.fsync(out);
            }

            // Explicit component: only our own receiver can get the result. The system fills in the
            // status extras, which needs a mutable PendingIntent on Android 12 and later.
            Intent statusIntent = new Intent(ACTION_STATUS).setClass(context, UpdateInstallReceiver.class);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= 31) {
                flags |= PendingIntent.FLAG_MUTABLE;
            }
            PendingIntent pending = PendingIntent.getBroadcast(context, id, statusIntent, flags);
            session.commit(pending.getIntentSender());
        } catch (IOException | RuntimeException e) {
            try {
                installer.abandonSession(id);
            } catch (RuntimeException ignored) {
                // Already closed
            }
            if (e instanceof IOException) {
                throw (IOException) e;
            }
            throw new IOException(e.getClass().getSimpleName());
        }
        return id;
    }

    @Override
    public void abandon(Context context, int sessionId) {
        try {
            context.getPackageManager().getPackageInstaller().abandonSession(sessionId);
        } catch (RuntimeException ignored) {
            // Already finished
        }
    }
}
