package com.limelight.utils;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;

import androidx.preference.PreferenceManager;
import androidx.test.core.app.ApplicationProvider;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class UiHelperLanguageTest {
    private static void setLanguage(String language) {
        Context context = ApplicationProvider.getApplicationContext();
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        prefs.edit().putString("list_languages", language).commit();
    }

    @Test
    public void unchangedLanguageDoesNotRecreate() {
        setLanguage("default");
        Activity activity = Robolectric.buildActivity(Activity.class).create().get();
        UiHelper.setLocale(activity);

        assertFalse(UiHelper.recreateIfLanguageChanged(activity));
    }

    @Test
    public void changedLanguageRecreatesTheActivity() {
        setLanguage("default");
        Activity activity = Robolectric.buildActivity(Activity.class).create().start().resume().get();
        UiHelper.setLocale(activity);

        setLanguage("tr");
        assertTrue(UiHelper.recreateIfLanguageChanged(activity));
        shadowOf(android.os.Looper.getMainLooper()).idle();
    }

    @Test
    public void activityThatNeverAppliedALanguageIsLeftAlone() {
        setLanguage("tr");
        Activity activity = Robolectric.buildActivity(Activity.class).create().get();

        assertFalse(UiHelper.recreateIfLanguageChanged(activity));
    }
}
