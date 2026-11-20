package com.limelight.utils;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

/**
 * Decides whether this app may install an update itself and which system screen lets the user allow it.
 */
public final class UpdateAccess {
    public enum Access {
        /** The install can start. */
        ALLOWED,
        /** Android 8+: the per-app "Install unknown apps" switch is off; its screen exists. */
        NEEDS_APP_SETTING,
        /** Android 5-7: the global "Unknown sources" switch is off; its screen exists. */
        NEEDS_GLOBAL_SETTING,
        /** Not allowed and the user cannot be sent to a screen that fixes it (some TVs). */
        UNAVAILABLE
    }

    private UpdateAccess() {}

    /** The decision itself, free of platform calls. */
    static Access decide(int sdk, boolean canRequestInstalls, boolean unknownSourcesOn, boolean settingsScreenExists) {
        if (sdk >= Build.VERSION_CODES.O) {
            if (canRequestInstalls) {
                return Access.ALLOWED;
            }
            return settingsScreenExists ? Access.NEEDS_APP_SETTING : Access.UNAVAILABLE;
        }
        if (unknownSourcesOn) {
            return Access.ALLOWED;
        }
        return settingsScreenExists ? Access.NEEDS_GLOBAL_SETTING : Access.UNAVAILABLE;
    }

    public static Access check(Context context) {
        PackageManager pm = context.getPackageManager();
        int sdk = Build.VERSION.SDK_INT;
        boolean can = false;
        boolean global = false;
        if (sdk >= Build.VERSION_CODES.O) {
            can = pm.canRequestPackageInstalls();
        } else {
            global = unknownSourcesEnabled(context);
        }
        Intent screen = settingsIntent(context, sdk >= Build.VERSION_CODES.O ? Access.NEEDS_APP_SETTING : Access.NEEDS_GLOBAL_SETTING);
        boolean exists = screen != null && screen.resolveActivity(pm) != null;
        return decide(sdk, can, global, exists);
    }

    @SuppressWarnings("deprecation")
    private static boolean unknownSourcesEnabled(Context context) {
        try {
            return Settings.Global.getInt(context.getContentResolver(), Settings.Global.INSTALL_NON_MARKET_APPS, 0) == 1;
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** The system screen that fixes the given access problem, or null. Only fixed system actions are used. */
    public static Intent settingsIntent(Context context, Access access) {
        Intent intent;
        if (access == Access.NEEDS_APP_SETTING && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            intent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + context.getPackageName()));
        } else if (access == Access.NEEDS_GLOBAL_SETTING) {
            intent = new Intent(Settings.ACTION_SECURITY_SETTINGS);
        } else {
            return null;
        }
        return intent;
    }
}
