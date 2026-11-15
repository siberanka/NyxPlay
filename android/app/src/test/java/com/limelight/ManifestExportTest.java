package com.limelight;

import static org.junit.Assert.assertEquals;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;

import androidx.test.core.app.ApplicationProvider;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.Set;
import java.util.TreeSet;

@Config(sdk = {33})
@RunWith(RobolectricTestRunner.class)
public class ManifestExportTest {
    private PackageInfo packageInfo() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        return context.getPackageManager().getPackageInfo(context.getPackageName(),
                PackageManager.GET_ACTIVITIES | PackageManager.GET_SERVICES
                        | PackageManager.GET_PROVIDERS | PackageManager.GET_RECEIVERS);
    }

    @Test
    public void onlyTheIntendedActivitiesAreExported() throws Exception {
        Set<String> exported = new TreeSet<>();
        for (ActivityInfo info : packageInfo().activities) {
            if (info.exported) {
                exported.add(info.name);
            }
        }

        Set<String> expected = new TreeSet<>();
        expected.add("com.limelight.PcView");
        expected.add("com.limelight.ShortcutTrampoline");
        expected.add("com.limelight.preferences.AddComputerManually");
        assertEquals(expected, exported);
    }

    @Test
    public void servicesAndReceiversAreNotExported() throws Exception {
        PackageInfo info = packageInfo();
        if (info.services != null) {
            for (ServiceInfo service : info.services) {
                if (service.name.startsWith("com.limelight.")) {
                    assertEquals(service.name, false, service.exported);
                }
            }
        }
        if (info.receivers != null) {
            for (ActivityInfo receiver : info.receivers) {
                if (receiver.name.startsWith("com.limelight.")) {
                    assertEquals(receiver.name, false, receiver.exported);
                }
            }
        }
    }

    @Test
    public void backupsAreDisabledAndTheRulesExcludeEverything() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        assertEquals(0, context.getApplicationInfo().flags & ApplicationInfo.FLAG_ALLOW_BACKUP);

        for (String rules : new String[]{"backup_rules", "backup_rules_s"}) {
            int id = context.getResources().getIdentifier(rules, "xml", context.getPackageName());
            android.content.res.XmlResourceParser parser = context.getResources().getXml(id);
            Set<String> excluded = new TreeSet<>();
            int event;
            while ((event = parser.next()) != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
                if (event == org.xmlpull.v1.XmlPullParser.START_TAG && "exclude".equals(parser.getName())) {
                    excluded.add(parser.getAttributeValue(null, "domain"));
                }
            }
            for (String domain : new String[]{"file", "database", "sharedpref"}) {
                assertEquals(rules + " must exclude " + domain, true, excluded.contains(domain));
            }
        }
    }

    @Test
    public void onlyThePosterProviderIsExported() throws Exception {
        Set<String> exported = new TreeSet<>();
        for (ProviderInfo provider : packageInfo().providers) {
            if (provider.exported) {
                exported.add(provider.name);
            }
        }

        Set<String> expected = new TreeSet<>();
        expected.add("com.limelight.PosterContentProvider");
        assertEquals(expected, exported);
    }
}
