package com.limelight.utils;

import android.content.Context;
import android.content.Intent;

import androidx.core.content.pm.ShortcutInfoCompat;
import androidx.core.content.pm.ShortcutManagerCompat;
import androidx.core.graphics.drawable.IconCompat;

import com.limelight.R;
import com.limelight.UpdateShortcutActivity;

import java.util.Collections;

/**
 * The launcher shortcuts "Check for updates" and, while an update is waiting, "Install update".
 * They start a non-exported trampoline activity inside this app, so no other app can use them.
 */
public final class UpdateShortcuts {
    public static final String ACTION_CHECK = "com.limelight.action.UPDATE_CHECK";
    public static final String ACTION_INSTALL = "com.limelight.action.UPDATE_INSTALL";
    public static final String EXTRA_VERSION = "update_version";

    static final String ID_CHECK = "nyxplay_update_check";
    static final String ID_INSTALL = "nyxplay_update_install";

    public enum Action {
        NONE,
        CHECK,
        INSTALL
    }

    private UpdateShortcuts() {}

    /**
     * Validates a shortcut intent. Only the two fixed actions are accepted; an install request must name
     * exactly the update that is currently waiting, otherwise it is treated as a plain check.
     */
    public static Action parse(Intent intent, String pendingVersion) {
        if (intent == null || intent.getAction() == null) {
            return Action.NONE;
        }
        String action = intent.getAction();
        if (ACTION_CHECK.equals(action)) {
            return Action.CHECK;
        }
        if (ACTION_INSTALL.equals(action)) {
            String version = intent.getStringExtra(EXTRA_VERSION);
            if (version != null && pendingVersion != null && version.equals(pendingVersion)
                    && UpdateChecker.parseInstalledVersion(version) != null && !version.contains("-")) {
                return Action.INSTALL;
            }
            return Action.CHECK;
        }
        return Action.NONE;
    }

    static Intent checkIntent(Context context) {
        return new Intent(ACTION_CHECK).setClass(context, UpdateShortcutActivity.class);
    }

    static Intent installIntent(Context context, String version) {
        return new Intent(ACTION_INSTALL).setClass(context, UpdateShortcutActivity.class).putExtra(EXTRA_VERSION, version);
    }

    /** Publishes the check shortcut, and the install shortcut when a version is waiting (null removes it). */
    public static void refresh(Context context, String pendingVersion) {
        try {
            Context app = context.getApplicationContext();
            IconCompat icon = IconCompat.createWithResource(app, R.mipmap.ic_launcher);
            ShortcutManagerCompat.pushDynamicShortcut(app, new ShortcutInfoCompat.Builder(app, ID_CHECK)
                    .setShortLabel(app.getString(R.string.shortcut_check_updates))
                    .setLongLabel(app.getString(R.string.shortcut_check_updates))
                    .setIcon(icon)
                    .setIntent(checkIntent(app))
                    .setRank(1)
                    .build());
            if (pendingVersion != null) {
                ShortcutManagerCompat.pushDynamicShortcut(app, new ShortcutInfoCompat.Builder(app, ID_INSTALL)
                        .setShortLabel(app.getString(R.string.shortcut_install_update_short))
                        .setLongLabel(app.getString(R.string.shortcut_install_update, pendingVersion))
                        .setIcon(icon)
                        .setIntent(installIntent(app, pendingVersion))
                        .setRank(0)
                        .build());
            } else {
                ShortcutManagerCompat.removeDynamicShortcuts(app, Collections.singletonList(ID_INSTALL));
            }
        } catch (RuntimeException e) {
            // Launchers without shortcut support, or the rate limit: the in-app entries still work
        }
    }
}
