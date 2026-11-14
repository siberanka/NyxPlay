package com.limelight.utils;

import android.content.Context;
import android.content.SharedPreferences;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;

/**
 * Per-install random value that marks launch intents created by this app.
 *
 * Exported activities accept intents from any app. Intents built by this app (launcher shortcuts,
 * TV channel entries, in-app hand-offs) carry this value, so they can be told apart from intents
 * that were crafted elsewhere. The value lives in app-private preferences, which are excluded from
 * backup and device transfer.
 */
public final class LaunchToken {
    public static final String EXTRA = "LaunchToken";

    private static final String PREFS_NAME = "launch_token";
    private static final String KEY_TOKEN = "token";
    private static final int TOKEN_BYTES = 24;

    private LaunchToken() {
    }

    public static synchronized String get(Context context) {
        Context appContext = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        SharedPreferences prefs = appContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        String token = prefs.getString(KEY_TOKEN, null);
        if (token == null || token.length() != TOKEN_BYTES * 2) {
            byte[] raw = new byte[TOKEN_BYTES];
            new SecureRandom().nextBytes(raw);

            StringBuilder sb = new StringBuilder(TOKEN_BYTES * 2);
            for (byte b : raw) {
                sb.append(String.format("%02x", b & 0xFF));
            }
            token = sb.toString();
            prefs.edit().putString(KEY_TOKEN, token).commit();
        }

        return token;
    }

    public static boolean matches(Context context, String candidate) {
        if (candidate == null) {
            return false;
        }

        return MessageDigest.isEqual(get(context).getBytes(StandardCharsets.UTF_8),
                candidate.getBytes(StandardCharsets.UTF_8));
    }
}
