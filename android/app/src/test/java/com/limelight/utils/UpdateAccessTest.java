package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import android.provider.Settings;

import androidx.test.core.app.ApplicationProvider;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
public class UpdateAccessTest {
    private final Context context = ApplicationProvider.getApplicationContext();

    private void registerScreen(UpdateAccess.Access access) {
        ResolveInfo info = new ResolveInfo();
        info.activityInfo = new ActivityInfo();
        info.activityInfo.packageName = "com.android.settings";
        info.activityInfo.name = "Settings";
        Shadows.shadowOf(context.getPackageManager()).addResolveInfoForIntent(UpdateAccess.settingsIntent(context, access), info);
    }

    // ---- The decision, for every API level family ----

    @Test
    public void decisionMatrix() {
        // Android 8+: per-app permission
        assertEquals(UpdateAccess.Access.ALLOWED, UpdateAccess.decide(26, true, false, true));
        assertEquals(UpdateAccess.Access.ALLOWED, UpdateAccess.decide(34, true, false, false));
        assertEquals(UpdateAccess.Access.NEEDS_APP_SETTING, UpdateAccess.decide(26, false, true, true));
        assertEquals(UpdateAccess.Access.NEEDS_APP_SETTING, UpdateAccess.decide(34, false, false, true));
        assertEquals(UpdateAccess.Access.UNAVAILABLE, UpdateAccess.decide(30, false, false, false));
        // Android 5-7: global switch
        assertEquals(UpdateAccess.Access.ALLOWED, UpdateAccess.decide(21, false, true, false));
        assertEquals(UpdateAccess.Access.ALLOWED, UpdateAccess.decide(25, true, true, true));
        assertEquals(UpdateAccess.Access.NEEDS_GLOBAL_SETTING, UpdateAccess.decide(21, false, false, true));
        assertEquals(UpdateAccess.Access.NEEDS_GLOBAL_SETTING, UpdateAccess.decide(25, true, false, true));
        assertEquals(UpdateAccess.Access.UNAVAILABLE, UpdateAccess.decide(23, false, false, false));
    }

    // ---- Platform glue per API level ----

    @Test
    @Config(sdk = {33})
    public void api33WithoutPermissionNeedsTheAppSetting() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        registerScreen(UpdateAccess.Access.NEEDS_APP_SETTING);
        assertEquals(UpdateAccess.Access.NEEDS_APP_SETTING, UpdateAccess.check(context));

        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(true);
        assertEquals(UpdateAccess.Access.ALLOWED, UpdateAccess.check(context));
    }

    @Test
    @Config(sdk = {26})
    public void api26UsesThePerAppPermission() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        registerScreen(UpdateAccess.Access.NEEDS_APP_SETTING);
        assertEquals(UpdateAccess.Access.NEEDS_APP_SETTING, UpdateAccess.check(context));
    }

    @Test
    @Config(sdk = {33})
    public void deviceWithoutTheScreenIsUnavailable() {
        Shadows.shadowOf(context.getPackageManager()).setCanRequestPackageInstalls(false);
        assertEquals(UpdateAccess.Access.UNAVAILABLE, UpdateAccess.check(context));
    }

    @Test
    @Config(sdk = {24})
    public void api24UsesTheGlobalUnknownSourcesSwitch() {
        Settings.Global.putInt(context.getContentResolver(), Settings.Global.INSTALL_NON_MARKET_APPS, 0);
        registerScreen(UpdateAccess.Access.NEEDS_GLOBAL_SETTING);
        assertEquals(UpdateAccess.Access.NEEDS_GLOBAL_SETTING, UpdateAccess.check(context));

        Settings.Global.putInt(context.getContentResolver(), Settings.Global.INSTALL_NON_MARKET_APPS, 1);
        assertEquals(UpdateAccess.Access.ALLOWED, UpdateAccess.check(context));
    }

    @Test
    @Config(sdk = {23})
    public void api23WithoutTheSecurityScreenIsUnavailable() {
        Settings.Global.putInt(context.getContentResolver(), Settings.Global.INSTALL_NON_MARKET_APPS, 0);
        assertEquals(UpdateAccess.Access.UNAVAILABLE, UpdateAccess.check(context));
    }

    // ---- Which screen is opened ----

    @Test
    @Config(sdk = {33})
    public void appSettingScreenNamesThisPackageOnly() {
        Intent i = UpdateAccess.settingsIntent(context, UpdateAccess.Access.NEEDS_APP_SETTING);
        assertEquals(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, i.getAction());
        assertEquals("package:" + context.getPackageName(), i.getData().toString());
        assertNull(i.getComponent());
    }

    @Test
    @Config(sdk = {24})
    public void legacyScreenIsTheSecuritySettings() {
        Intent i = UpdateAccess.settingsIntent(context, UpdateAccess.Access.NEEDS_GLOBAL_SETTING);
        assertNotNull(i);
        assertEquals(Settings.ACTION_SECURITY_SETTINGS, i.getAction());
    }

    @Test
    @Config(sdk = {33})
    public void nothingToOpenWhenAllowedOrUnavailable() {
        assertNull(UpdateAccess.settingsIntent(context, UpdateAccess.Access.ALLOWED));
        assertNull(UpdateAccess.settingsIntent(context, UpdateAccess.Access.UNAVAILABLE));
    }
}
