package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.mockwebserver.RecordedRequest;
import okhttp3.mockwebserver.SocketPolicy;

@Config(sdk = {33})
@RunWith(RobolectricTestRunner.class)
public class UpdateCheckerTest {
    private static final String INSTALLED_OLD = "2026.11.1.1";

    private MockWebServer server;

    @Before
    public void setUp() throws Exception {
        server = new MockWebServer();
        server.start();
    }

    @After
    public void tearDown() throws Exception {
        server.shutdown();
    }

    private static String resource(String name) throws Exception {
        try (InputStream in = UpdateCheckerTest.class.getResourceAsStream("/update/" + name)) {
            assertNotNull("missing test resource " + name, in);
            return new String(readAll(in), StandardCharsets.UTF_8);
        }
    }

    private static byte[] readAll(InputStream in) throws Exception {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private UpdateChecker checker(String installed, String... abis) {
        OkHttpClient client = new OkHttpClient.Builder()
                .connectTimeout(2, TimeUnit.SECONDS)
                .readTimeout(1, TimeUnit.SECONDS)
                .build();
        return new UpdateChecker(client, server.url("/github").toString(), server.url("/gitlab").toString(),
                installed, Arrays.asList(abis));
    }

    private static int[] v(String s) {
        return UpdateChecker.parseInstalledVersion(s);
    }

    // ---- Version comparison ----

    @Test
    public void compareEqualOlderNewer() {
        assertEquals(0, UpdateChecker.compareVersions(v("2026.11.19.1"), v("2026.11.19.1")));
        assertEquals(-1, UpdateChecker.compareVersions(v("2026.11.19.1"), v("2026.11.19.2")));
        assertEquals(1, UpdateChecker.compareVersions(v("2026.11.19.10"), v("2026.11.19.9")));
        assertEquals(1, UpdateChecker.compareVersions(v("2026.12.1.1"), v("2026.11.30.9")));
        assertEquals(-1, UpdateChecker.compareVersions(v("2025.12.31.9"), v("2026.1.1.1")));
    }

    @Test
    public void compareDifferentComponentCounts() {
        assertEquals(0, UpdateChecker.compareVersions(v("2026.11.19"), v("2026.11.19.0")));
        assertEquals(-1, UpdateChecker.compareVersions(v("2026.11.19"), v("2026.11.19.1")));
        assertEquals(1, UpdateChecker.compareVersions(v("2026.11.19.1.1"), v("2026.11.19.1")));
    }

    @Test
    public void installedVersionIgnoresSuffix() {
        assertEquals(0, UpdateChecker.compareVersions(v("2026.11.19.1-debug"), v("2026.11.19.1")));
        assertEquals(0, UpdateChecker.compareVersions(v("2026.11.19.1-debug-foo"), v("2026.11.19.1")));
        assertEquals(0, UpdateChecker.compareVersions(v(" 2026.11.19.1 "), v("2026.11.19.1")));
    }

    @Test
    public void junkInstalledVersionIsNotParsable() {
        assertNull(v(null));
        assertNull(v(""));
        assertNull(v("dev"));
        assertNull(v("-debug"));
        assertNull(v("v2026.11.19.1"));
        assertNull(v("2026..11"));
        assertNull(v("2026.11.19.1abc"));
        assertNull(v("99999999999.1"));
    }

    @Test
    public void unparsableInstalledVersionNeverOffersUpdate() throws Exception {
        server.enqueue(new MockResponse().setBody(resource("github-latest.json")));
        UpdateChecker.Result result = checker("dev", "arm64-v8a").check();
        assertEquals(UpdateChecker.Status.UP_TO_DATE, result.status);
        assertEquals("no request must be made", 0, server.getRequestCount());
    }

    // ---- Tag filter ----

    @Test
    public void tagFilter() {
        assertTrue(UpdateChecker.isReleaseTag("v2026.11.19.1"));
        assertTrue(UpdateChecker.isReleaseTag("2026.1.5.12"));
        assertFalse(UpdateChecker.isReleaseTag("host-v2026.11.19.1"));
        assertFalse(UpdateChecker.isReleaseTag("v2026.11.19"));
        assertFalse(UpdateChecker.isReleaseTag("v2026.11.19.1-rc1"));
        assertFalse(UpdateChecker.isReleaseTag("v2.1.0.1"));
        assertFalse(UpdateChecker.isReleaseTag("v2026.11.19.1\n"));
        assertFalse(UpdateChecker.isReleaseTag("latest"));
        assertFalse(UpdateChecker.isReleaseTag(""));
        assertFalse(UpdateChecker.isReleaseTag(null));
        assertFalse(UpdateChecker.isReleaseTag("v2026.11.19.12345678901"));
    }

    @Test
    public void draftsAndPrereleasesAreSkipped() {
        String draft = "{\"tag_name\":\"v2026.12.1.1\",\"draft\":true,\"prerelease\":false}";
        String pre = "{\"tag_name\":\"v2026.12.1.1\",\"draft\":false,\"prerelease\":true}";
        String hostTag = "{\"tag_name\":\"host-v2026.12.1.1\",\"draft\":false,\"prerelease\":false}";
        assertNull(UpdateChecker.parseGitHub(draft));
        assertNull(UpdateChecker.parseGitHub(pre));
        assertNull(UpdateChecker.parseGitHub(hostTag));
        assertNotNull(UpdateChecker.parseGitHub("{\"tag_name\":\"v2026.12.1.1\",\"draft\":false,\"prerelease\":false}"));
        assertNull(UpdateChecker.parseGitLab("{\"tag_name\":\"v2026.12.1.1\",\"upcoming_release\":true}"));
    }

    // ---- JSON samples ----

    @Test
    public void parsesRealGitHubSample() throws Exception {
        UpdateChecker.Release r = UpdateChecker.parseGitHub(resource("github-latest.json"));
        assertNotNull(r);
        assertEquals("v2026.11.19.1", r.version);
        assertEquals("https://github.com/siberanka/NyxPlay/releases/tag/v2026.11.19.1", r.pageUrl);
        assertEquals(7, r.assets.size());
        assertTrue(r.notes.contains("Security and reliability update"));
    }

    @Test
    public void parsesGitLabSample() throws Exception {
        UpdateChecker.Release r = UpdateChecker.parseGitLab(resource("gitlab-latest.json"));
        assertNotNull(r);
        assertEquals("v2026.11.19.1", r.version);
        assertEquals("https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.11.19.1", r.pageUrl);
        assertEquals(4, r.assets.size());
    }

    @Test
    public void malformedJsonIsRejected() {
        assertNull(UpdateChecker.parseGitHub("not json"));
        assertNull(UpdateChecker.parseGitHub("[]"));
        assertNull(UpdateChecker.parseGitHub("{}"));
        assertNull(UpdateChecker.parseGitHub("{\"tag_name\":42}"));
        assertNull(UpdateChecker.parseGitLab("<html></html>"));
    }

    // ---- Fetching and fallback ----

    @Test
    public void githubAnswerIsUsedFirstAndRequestIsMinimal() throws Exception {
        server.enqueue(new MockResponse().setBody(resource("github-latest.json")));

        UpdateChecker.Result result = checker(INSTALLED_OLD + "-debug", "arm64-v8a", "armeabi-v7a").check();

        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, result.status);
        assertEquals("2026.11.19.1", result.version);
        assertEquals("https://github.com/siberanka/NyxPlay/releases/download/v2026.11.19.1/NyxPlay-2026.11.19.1-arm64-v8a-release.apk",
                result.downloadUrl);
        assertEquals("https://github.com/siberanka/NyxPlay/releases/tag/v2026.11.19.1", result.pageUrl);
        assertFalse(result.notes.contains("|"));
        assertFalse(result.notes.contains("#"));
        assertTrue(result.notes.length() <= UpdateChecker.MAX_NOTES_CHARS + 1);

        assertEquals(1, server.getRequestCount());
        RecordedRequest request = server.takeRequest();
        assertEquals("/github", request.getPath());
        assertEquals("NyxPlay-Android/" + INSTALLED_OLD + "-debug", request.getHeader("User-Agent"));
        assertNull(request.getHeader("Authorization"));
        assertNull(request.getHeader("Cookie"));
    }

    @Test
    public void sameVersionIsUpToDate() throws Exception {
        server.enqueue(new MockResponse().setBody(resource("github-latest.json")));
        UpdateChecker.Result result = checker("2026.11.19.1-debug", "arm64-v8a").check();
        assertEquals(UpdateChecker.Status.UP_TO_DATE, result.status);
    }

    @Test
    public void newerInstalledVersionIsUpToDate() throws Exception {
        server.enqueue(new MockResponse().setBody(resource("github-latest.json")));
        UpdateChecker.Result result = checker("2026.12.1.1", "arm64-v8a").check();
        assertEquals(UpdateChecker.Status.UP_TO_DATE, result.status);
    }

    @Test
    public void rateLimitedGitHubFallsBackToGitLab() throws Exception {
        server.enqueue(new MockResponse().setResponseCode(403).setBody("{\"message\":\"rate limit exceeded\"}"));
        server.enqueue(new MockResponse().setBody(resource("gitlab-latest.json")));

        UpdateChecker.Result result = checker(INSTALLED_OLD, "x86_64").check();

        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, result.status);
        assertEquals("2026.11.19.1", result.version);
        assertTrue(result.downloadUrl, result.downloadUrl.startsWith("https://gitlab.com/siberanka/NyxPlay/"));
        assertTrue(result.downloadUrl.contains("x86_64"));
        assertEquals("https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.11.19.1", result.pageUrl);
        assertEquals("/github", server.takeRequest().getPath());
        assertEquals("/gitlab", server.takeRequest().getPath());
    }

    @Test
    public void tooManyRequestsAndServerErrorsFallBack() throws Exception {
        server.enqueue(new MockResponse().setResponseCode(429));
        server.enqueue(new MockResponse().setBody(resource("gitlab-latest.json")));
        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, checker(INSTALLED_OLD, "arm64-v8a").check().status);

        server.enqueue(new MockResponse().setResponseCode(500));
        server.enqueue(new MockResponse().setBody(resource("gitlab-latest.json")));
        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, checker(INSTALLED_OLD, "arm64-v8a").check().status);
    }

    @Test
    public void unusableGitHubDataFallsBackToGitLab() throws Exception {
        server.enqueue(new MockResponse().setBody("{\"tag_name\":\"host-v2026.12.1.1\"}"));
        server.enqueue(new MockResponse().setBody(resource("gitlab-latest.json")));
        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, checker(INSTALLED_OLD, "arm64-v8a").check().status);
    }

    @Test
    public void bothSourcesHttpErrorsIsBadResponse() {
        server.enqueue(new MockResponse().setResponseCode(403));
        server.enqueue(new MockResponse().setResponseCode(404).setBody("{\"message\":\"404 Project Not Found\"}"));

        UpdateChecker.Result result = checker(INSTALLED_OLD, "arm64-v8a").check();

        assertEquals(UpdateChecker.Status.FAILED, result.status);
        assertEquals(UpdateChecker.Failure.BAD_RESPONSE, result.failure);
        assertEquals(2, server.getRequestCount());
    }

    @Test
    public void bothSourcesUnreachableIsNoConnection() throws Exception {
        // Nothing listens on a port that was just released
        MockWebServer dead = new MockWebServer();
        dead.start();
        String url = dead.url("/x").toString();
        dead.shutdown();
        OkHttpClient client = new OkHttpClient.Builder().connectTimeout(1, TimeUnit.SECONDS).build();

        UpdateChecker.Result result = new UpdateChecker(client, url, url, INSTALLED_OLD,
                Collections.singletonList("arm64-v8a")).check();

        assertEquals(UpdateChecker.Status.FAILED, result.status);
        assertEquals(UpdateChecker.Failure.NO_CONNECTION, result.failure);
    }

    @Test
    public void oversizedBodyIsRejected() {
        byte[] big = new byte[UpdateChecker.MAX_BODY_BYTES + 10];
        Arrays.fill(big, (byte) ' ');
        server.enqueue(new MockResponse().setBody(new okio.Buffer().write(big)));
        server.enqueue(new MockResponse().setBody(new okio.Buffer().write(big)));

        UpdateChecker.Result result = checker(INSTALLED_OLD, "arm64-v8a").check();

        assertEquals(UpdateChecker.Status.FAILED, result.status);
        assertEquals(UpdateChecker.Failure.BAD_RESPONSE, result.failure);
    }

    @Test
    public void oversizedChunkedBodyIsRejected() {
        byte[] big = new byte[UpdateChecker.MAX_BODY_BYTES + 10];
        Arrays.fill(big, (byte) ' ');
        server.enqueue(new MockResponse().setChunkedBody(new okio.Buffer().write(big), 8192));
        server.enqueue(new MockResponse().setResponseCode(404));

        assertEquals(UpdateChecker.Status.FAILED, checker(INSTALLED_OLD, "arm64-v8a").check().status);
    }

    @Test
    public void malformedJsonFromBothSourcesFails() {
        server.enqueue(new MockResponse().setBody("{{{{"));
        server.enqueue(new MockResponse().setBody("<html>login</html>"));

        UpdateChecker.Result result = checker(INSTALLED_OLD, "arm64-v8a").check();

        assertEquals(UpdateChecker.Status.FAILED, result.status);
        assertEquals(UpdateChecker.Failure.BAD_RESPONSE, result.failure);
    }

    @Test
    public void timeoutOnGitHubFallsBackToGitLab() throws Exception {
        server.enqueue(new MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE));
        server.enqueue(new MockResponse().setBody(resource("gitlab-latest.json")));

        UpdateChecker.Result result = checker(INSTALLED_OLD, "arm64-v8a").check();

        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, result.status);
    }

    @Test
    public void timeoutOnBothIsNoConnection() {
        server.enqueue(new MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE));
        server.enqueue(new MockResponse().setSocketPolicy(SocketPolicy.NO_RESPONSE));

        UpdateChecker.Result result = checker(INSTALLED_OLD, "arm64-v8a").check();

        assertEquals(UpdateChecker.Status.FAILED, result.status);
        assertEquals(UpdateChecker.Failure.NO_CONNECTION, result.failure);
    }

    @Test
    public void redirectToUnsafeSchemeIsNotFollowed() {
        // MockWebServer is plain http, so a redirect to an https location on another host is a plain failure
        server.enqueue(new MockResponse().setResponseCode(302).setHeader("Location", "https://example.invalid/x"));
        server.enqueue(new MockResponse().setResponseCode(404));
        assertEquals(UpdateChecker.Status.FAILED, checker(INSTALLED_OLD, "arm64-v8a").check().status);
    }

    @Test
    public void releaseWithoutMatchingApkOffersTheReleasePage() throws Exception {
        server.enqueue(new MockResponse().setBody(resource("github-latest.json")));
        UpdateChecker.Result result = checker(INSTALLED_OLD, "mips").check();
        assertEquals(UpdateChecker.Status.UPDATE_AVAILABLE, result.status);
        assertEquals(result.pageUrl, result.downloadUrl);
    }

    // ---- ABI selection ----

    private static List<String[]> assets() {
        String base = "https://github.com/siberanka/NyxPlay/releases/download/v2026.11.19.1/";
        List<String[]> list = new ArrayList<>();
        for (String abi : new String[] {"arm64-v8a", "armeabi-v7a", "x86", "x86_64"}) {
            String name = "NyxPlay-2026.11.19.1-" + abi + "-release.apk";
            list.add(new String[] {name, base + name});
        }
        list.add(new String[] {"NyxPlayHost-2026.11.19.1-windows-x64.exe", base + "NyxPlayHost-2026.11.19.1-windows-x64.exe"});
        return list;
    }

    @Test
    public void arm64PhonePicksArm64() {
        String url = UpdateChecker.selectApkUrl(assets(), Arrays.asList("arm64-v8a", "armeabi-v7a", "armeabi"));
        assertTrue(url, url.endsWith("-arm64-v8a-release.apk"));
    }

    @Test
    public void x86_64EmulatorPicksX86_64NotX86() {
        String url = UpdateChecker.selectApkUrl(assets(), Arrays.asList("x86_64", "x86", "arm64-v8a"));
        assertTrue(url, url.endsWith("-x86_64-release.apk"));
        url = UpdateChecker.selectApkUrl(assets(), Arrays.asList("x86"));
        assertTrue(url, url.endsWith("-x86-release.apk"));
    }

    @Test
    public void firstAbiWithAnAssetWins() {
        List<String[]> onlyV7 = new ArrayList<>();
        for (String[] a : assets()) {
            if (a[0].contains("armeabi-v7a")) {
                onlyV7.add(a);
            }
        }
        String url = UpdateChecker.selectApkUrl(onlyV7, Arrays.asList("arm64-v8a", "armeabi-v7a"));
        assertTrue(url, url.endsWith("-armeabi-v7a-release.apk"));
    }

    @Test
    public void deviceWithoutMatchingAbiGetsNoApk() {
        assertNull(UpdateChecker.selectApkUrl(assets(), Arrays.asList("mips", "mips64")));
        assertNull(UpdateChecker.selectApkUrl(assets(), Collections.<String>emptyList()));
        assertNull(UpdateChecker.selectApkUrl(null, Arrays.asList("arm64-v8a")));
    }

    @Test
    public void assetsOnForeignHostsAreIgnored() {
        List<String[]> evil = new ArrayList<>();
        evil.add(new String[] {"NyxPlay-1-arm64-v8a-release.apk", "https://evil.example/NyxPlay-1-arm64-v8a-release.apk"});
        evil.add(new String[] {"NyxPlay-1-arm64-v8a-release.apk", "http://github.com/siberanka/NyxPlay/x.apk"});
        assertNull(UpdateChecker.selectApkUrl(evil, Arrays.asList("arm64-v8a")));
    }

    // ---- URL allow-list ----

    @Test
    public void allowedUrls() {
        assertTrue(UpdateChecker.isAllowedUrl("https://github.com/siberanka/NyxPlay/releases"));
        assertTrue(UpdateChecker.isAllowedUrl("https://github.com/siberanka/NyxPlay/releases/download/v1/a.apk"));
        assertTrue(UpdateChecker.isAllowedUrl("https://github.com/siberanka/nyxplay"));
        assertTrue(UpdateChecker.isAllowedUrl("https://objects.githubusercontent.com/github-production-release-asset/1?X-Amz=1"));
        assertTrue(UpdateChecker.isAllowedUrl("https://release-assets.githubusercontent.com/github-production-release-asset/1"));
        assertTrue(UpdateChecker.isAllowedUrl("https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.11.19.1"));
    }

    @Test
    public void rejectedUrls() {
        assertFalse(UpdateChecker.isAllowedUrl(null));
        assertFalse(UpdateChecker.isAllowedUrl(""));
        assertFalse(UpdateChecker.isAllowedUrl("http://github.com/siberanka/NyxPlay/releases"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com/other/NyxPlay/releases"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com/siberanka/NyxPlayEvil/releases"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com/siberanka/NyxPlay/../../evil/x"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com.evil.example/siberanka/NyxPlay/"));
        assertFalse(UpdateChecker.isAllowedUrl("https://evil.example/https://github.com/siberanka/NyxPlay/"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com@evil.example/siberanka/NyxPlay/"));
        assertFalse(UpdateChecker.isAllowedUrl("https://user:pw@github.com/siberanka/NyxPlay/"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com:8443/siberanka/NyxPlay/"));
        assertFalse(UpdateChecker.isAllowedUrl("https://objects.githubusercontent.com.evil.example/x"));
        assertFalse(UpdateChecker.isAllowedUrl("https://githubusercontent.com/x"));
        assertFalse(UpdateChecker.isAllowedUrl("https://gitlab.com/other/NyxPlay/"));
        assertFalse(UpdateChecker.isAllowedUrl("javascript:alert(1)"));
        assertFalse(UpdateChecker.isAllowedUrl("intent://github.com/siberanka/NyxPlay/#Intent;end"));
        assertFalse(UpdateChecker.isAllowedUrl("file:///sdcard/x.apk"));
        assertFalse(UpdateChecker.isAllowedUrl("https://github.com/siberanka/NyxPlay/ x"));
    }

    @Test
    public void foreignPageUrlFallsBackToFixedReleasePage() {
        server.enqueue(new MockResponse().setBody(
                "{\"tag_name\":\"v2026.12.1.1\",\"draft\":false,\"prerelease\":false,"
                        + "\"html_url\":\"https://evil.example/phish\",\"assets\":[]}"));
        UpdateChecker.Result result = checker(INSTALLED_OLD, "arm64-v8a").check();
        assertEquals(UpdateChecker.RELEASES_PAGE_URL, result.pageUrl);
        assertEquals(UpdateChecker.RELEASES_PAGE_URL, result.downloadUrl);
    }

    // ---- Release notes ----

    @Test
    public void notesAreStrippedOfMarkup() {
        String md = "## Title\n\nSecurity **update** with `code` and [a link](https://evil.example/x).\n"
                + "<script>alert(1)</script><b>bold</b> text\n- first item\n* second_item here\n"
                + "| a | b |\n|---|---|\n```\nignored block\n```\n![img](https://x/y.png)\n";
        String out = UpdateChecker.releaseNotesExcerpt(md, 600);
        assertEquals("Security update with code and a link.\nalert(1)bold text\n- first item\n- second_item here\nimg", out);
    }

    @Test
    public void notesAreCappedAtAWordBoundary() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 300; i++) {
            sb.append("word").append(i).append(' ');
        }
        String out = UpdateChecker.releaseNotesExcerpt(sb.toString(), 600);
        assertTrue(out.length() <= 601);
        assertTrue(out.endsWith("\u2026"));
        assertFalse(out.substring(0, out.length() - 1).endsWith(" "));
    }

    @Test
    public void notesHandleNullAndEmpty() {
        assertEquals("", UpdateChecker.releaseNotesExcerpt(null, 600));
        assertEquals("", UpdateChecker.releaseNotesExcerpt("", 600));
        assertEquals("", UpdateChecker.releaseNotesExcerpt("# only a title", 600));
    }
}
