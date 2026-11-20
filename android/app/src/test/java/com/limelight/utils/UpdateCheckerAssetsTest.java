package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Asset selection and the offer record used by the in-app install. */
@Config(sdk = {33})
@RunWith(RobolectricTestRunner.class)
public class UpdateCheckerAssetsTest {
    private static final String BASE = "https://github.com/siberanka/NyxPlay/releases/download/v2026.12.1.1/";

    private static List<String[]> assets() {
        List<String[]> list = new ArrayList<>();
        list.add(new String[] {"NyxPlay-2026.12.1.1-arm64-v8a-release.apk", BASE + "NyxPlay-2026.12.1.1-arm64-v8a-release.apk"});
        list.add(new String[] {"NyxPlay-2026.12.1.1-x86_64-release.apk", BASE + "NyxPlay-2026.12.1.1-x86_64-release.apk"});
        list.add(new String[] {"SHA256SUMS.txt", BASE + "SHA256SUMS.txt"});
        list.add(new String[] {"SHA256SUMS.txt.sig", BASE + "SHA256SUMS.txt.sig"});
        return list;
    }

    private static UpdateChecker.Result offer(List<String[]> assets, String abi) {
        String[] apk = UpdateChecker.selectApkAsset(assets, Arrays.asList(abi));
        return UpdateChecker.Result.update("2026.12.1.1", "n", apk[1], "https://github.com/siberanka/NyxPlay/releases/tag/v2026.12.1.1",
                apk[0], UpdateChecker.findAssetUrl(assets, UpdateChecker.SUMS_NAME), UpdateChecker.findAssetUrl(assets, UpdateChecker.SIG_NAME));
    }

    @Test
    public void apkAssetCarriesNameAndUrl() {
        String[] asset = UpdateChecker.selectApkAsset(assets(), Arrays.asList("x86_64"));
        assertEquals("NyxPlay-2026.12.1.1-x86_64-release.apk", asset[0]);
        assertEquals(BASE + asset[0], asset[1]);
        assertEquals(asset[1], UpdateChecker.selectApkUrl(assets(), Arrays.asList("x86_64")));
        assertNull(UpdateChecker.selectApkAsset(assets(), Arrays.asList("mips")));
    }

    @Test
    public void sumsAndSignatureAreFoundByExactName() {
        assertEquals(BASE + "SHA256SUMS.txt", UpdateChecker.findAssetUrl(assets(), "SHA256SUMS.txt"));
        assertEquals(BASE + "SHA256SUMS.txt.sig", UpdateChecker.findAssetUrl(assets(), "SHA256SUMS.txt.sig"));
        assertNull(UpdateChecker.findAssetUrl(assets(), "sha256sums.txt"));
        List<String[]> foreign = new ArrayList<>();
        foreign.add(new String[] {"SHA256SUMS.txt", "https://evil.example/SHA256SUMS.txt"});
        assertNull(UpdateChecker.findAssetUrl(foreign, "SHA256SUMS.txt"));
    }

    @Test
    public void completeOfferCanBeInstalledInTheApp() {
        assertTrue(offer(assets(), "x86_64").canInstallInApp());
    }

    @Test
    public void offerWithoutSignatureOrSumsCannot() {
        List<String[]> noSig = assets();
        noSig.remove(3);
        assertFalse(offer(noSig, "x86_64").canInstallInApp());
        List<String[]> noSums = assets();
        noSums.remove(2);
        assertFalse(offer(noSums, "x86_64").canInstallInApp());
        assertFalse(UpdateChecker.Result.update("2026.12.1.1", "", BASE + "a.apk", BASE).canInstallInApp());
        assertFalse(UpdateChecker.Result.upToDate("2026.12.1.1").canInstallInApp());
    }

    @Test
    public void assetNameMustContainTheOfferedVersion() {
        UpdateChecker.Result wrong = UpdateChecker.Result.update("2026.12.2.1", "", BASE + "NyxPlay-2026.12.1.1-x86_64-release.apk",
                BASE, "NyxPlay-2026.12.1.1-x86_64-release.apk", BASE + "SHA256SUMS.txt", BASE + "SHA256SUMS.txt.sig");
        assertFalse(wrong.canInstallInApp());
    }

    @Test
    public void assetNamesAreBoundedToSafeCharacters() {
        assertTrue(UpdateChecker.isSafeAssetName("NyxPlay-2026.12.1.1-x86_64-release.apk"));
        assertFalse(UpdateChecker.isSafeAssetName("../x.apk"));
        assertFalse(UpdateChecker.isSafeAssetName("a/b.apk"));
        assertFalse(UpdateChecker.isSafeAssetName("a\\b.apk"));
        assertFalse(UpdateChecker.isSafeAssetName("a b.apk"));
        assertFalse(UpdateChecker.isSafeAssetName(""));
        assertFalse(UpdateChecker.isSafeAssetName(null));
        List<String[]> odd = new ArrayList<>();
        odd.add(new String[] {"NyxPlay-..-x86_64-.apk", BASE + "x.apk"});
        assertNull(UpdateChecker.selectApkAsset(odd, Arrays.asList("x86_64")));
    }

    @Test
    public void offerSurvivesTheStateFile() {
        UpdateChecker.Result original = offer(assets(), "arm64-v8a");
        UpdateChecker.Result copy = UpdateChecker.Result.fromJson(original.toJson());
        assertNotNull(copy);
        assertEquals(original.version, copy.version);
        assertEquals(original.apkName, copy.apkName);
        assertEquals(original.sumsUrl, copy.sumsUrl);
        assertEquals(original.sigUrl, copy.sigUrl);
        assertEquals(original.downloadUrl, copy.downloadUrl);
        assertTrue(copy.canInstallInApp());

        // an offer without a signed list also round-trips and stays not installable
        UpdateChecker.Result plain = UpdateChecker.Result.update("2026.12.1.1", "", BASE + "a.apk", BASE);
        UpdateChecker.Result plainCopy = UpdateChecker.Result.fromJson(plain.toJson());
        assertNotNull(plainCopy);
        assertFalse(plainCopy.canInstallInApp());
    }

    @Test
    public void tamperedStateFileIsRejected() {
        assertNull(UpdateChecker.Result.fromJson("{"));
        assertNull(UpdateChecker.Result.fromJson(""));
        String good = offer(assets(), "x86_64").toJson();
        assertNull(UpdateChecker.Result.fromJson(good.replace("github.com", "evil.example")));
        assertNull(UpdateChecker.Result.fromJson(good.replace("\"version\":\"2026.12.1.1\"", "\"version\":\"../../1\"")));
        assertNull(UpdateChecker.Result.fromJson(good.replace("\"version\":\"2026.12.1.1\"", "\"version\":\"2026.12.1.1-debug\"")));

        // a swapped checksum location can never pass the install checks
        org.json.JSONObject o;
        try {
            o = new org.json.JSONObject(good);
            o.put("sums", "https://evil.example/s");
        } catch (org.json.JSONException e) {
            throw new AssertionError(e);
        }
        UpdateChecker.Result swapped = UpdateChecker.Result.fromJson(o.toString());
        assertTrue(swapped == null || !swapped.canInstallInApp());
    }
}
