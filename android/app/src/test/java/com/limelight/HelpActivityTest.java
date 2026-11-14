package com.limelight;

import static org.junit.Assert.assertTrue;

import android.app.Application;
import android.content.Intent;
import android.net.Uri;

import androidx.test.core.app.ApplicationProvider;

import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@Config(sdk = {33})
@RunWith(RobolectricTestRunner.class)
public class HelpActivityTest {
    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    @Test
    public void closesWithoutData() {
        Application app = ApplicationProvider.getApplicationContext();
        HelpActivity activity = Robolectric.buildActivity(HelpActivity.class, new Intent(app, HelpActivity.class)).create().get();
        assertTrue(activity.isFinishing());
    }

    @Test
    public void closesForNonHttpsData() {
        Application app = ApplicationProvider.getApplicationContext();
        Intent intent = new Intent(app, HelpActivity.class).setData(Uri.parse("file:///data/local/tmp/x.html"));
        HelpActivity activity = Robolectric.buildActivity(HelpActivity.class, intent).create().get();
        assertTrue(activity.isFinishing());

        intent = new Intent(app, HelpActivity.class).setData(Uri.parse("http://example.com/"));
        activity = Robolectric.buildActivity(HelpActivity.class, intent).create().get();
        assertTrue(activity.isFinishing());
    }
}
