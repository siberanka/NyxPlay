package com.limelight.utils;

import android.util.Log;

import com.limelight.BuildConfig;

import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.ResponseBody;
import okio.BufferedSource;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Looks up the newest published NyxPlay release. GitHub is asked first and the GitLab mirror is
 * used when GitHub cannot give a usable answer. Nothing but a fixed User-Agent is sent.
 *
 * check() never throws: every problem is reported as a FAILED result.
 */
public class UpdateChecker {
    private static final String TAG = "NyxPlayUpdate";

    public static final String GITHUB_LATEST_URL = "https://api.github.com/repos/siberanka/NyxPlay/releases/latest";
    public static final String GITLAB_LATEST_URL = "https://gitlab.com/api/v4/projects/siberanka%2FNyxPlay/releases/permalink/latest";
    public static final String RELEASES_PAGE_URL = "https://github.com/siberanka/NyxPlay/releases";

    public static final String SUMS_NAME = "SHA256SUMS.txt";
    public static final String SIG_NAME = "SHA256SUMS.txt.sig";

    // Debug builds can point the release lookup at a local test server; empty (and ignored) in release builds
    private static final String TEST_BASE = BuildConfig.DEBUG ? BuildConfig.UPDATE_TEST_BASE : "";

    static final int MAX_BODY_BYTES = 1024 * 1024;
    static final int MAX_NOTES_CHARS = 600;
    static final int TIMEOUT_SECONDS = 10;

    private static final Pattern TAG_PATTERN = Pattern.compile("^v?(\\d{4})\\.(\\d{1,2})\\.(\\d{1,2})\\.(\\d{1,9})$");
    private static final Pattern VERSION_PREFIX = Pattern.compile("^\\d{1,9}(?:\\.\\d{1,9}){0,5}$");

    public enum Status {
        UPDATE_AVAILABLE,
        UP_TO_DATE,
        FAILED
    }

    public enum Failure {
        NO_CONNECTION,
        BAD_RESPONSE
    }

    public static final class Result {
        public final Status status;
        public final String version;
        public final String notes;
        public final String downloadUrl;
        public final String pageUrl;
        public final Failure failure;
        /** File name of the matching APK asset, or null when only a page is known. */
        public final String apkName;
        /** Locations of the signed checksum list and its signature; null when the release has none. */
        public final String sumsUrl;
        public final String sigUrl;

        private Result(Status status, String version, String notes, String downloadUrl, String pageUrl, Failure failure,
                       String apkName, String sumsUrl, String sigUrl) {
            this.status = status;
            this.version = version;
            this.notes = notes;
            this.downloadUrl = downloadUrl;
            this.pageUrl = pageUrl;
            this.failure = failure;
            this.apkName = apkName;
            this.sumsUrl = sumsUrl;
            this.sigUrl = sigUrl;
        }

        public static Result update(String version, String notes, String downloadUrl, String pageUrl) {
            return new Result(Status.UPDATE_AVAILABLE, version, notes, downloadUrl, pageUrl, null, null, null, null);
        }

        public static Result update(String version, String notes, String downloadUrl, String pageUrl,
                                    String apkName, String sumsUrl, String sigUrl) {
            return new Result(Status.UPDATE_AVAILABLE, version, notes, downloadUrl, pageUrl, null, apkName, sumsUrl, sigUrl);
        }

        public static Result upToDate(String version) {
            return new Result(Status.UP_TO_DATE, version, "", null, null, null, null, null, null);
        }

        public static Result failed(Failure failure) {
            return new Result(Status.FAILED, null, "", null, null, failure, null, null, null);
        }

        /** True when the APK, the signed checksum list and its signature are all known and allowed. */
        public boolean canInstallInApp() {
            return status == Status.UPDATE_AVAILABLE && version != null && apkName != null && sumsUrl != null && sigUrl != null
                    && isAllowedUrl(downloadUrl) && isAllowedUrl(sumsUrl) && isAllowedUrl(sigUrl)
                    && isSafeAssetName(apkName) && apkName.contains(version);
        }

        /** Compact form for the private update state file. */
        public String toJson() {
            try {
                JSONObject o = new JSONObject();
                o.put("version", version);
                o.put("notes", notes == null ? "" : notes);
                o.put("download", downloadUrl);
                o.put("page", pageUrl);
                o.put("apk", apkName == null ? JSONObject.NULL : apkName);
                o.put("sums", sumsUrl == null ? JSONObject.NULL : sumsUrl);
                o.put("sig", sigUrl == null ? JSONObject.NULL : sigUrl);
                return o.toString();
            } catch (Exception e) {
                return "";
            }
        }

        /** Rebuilds an offer from toJson(); every field is validated again. Null when unusable. */
        public static Result fromJson(String json) {
            try {
                JSONObject o = new JSONObject(json);
                String version = o.optString("version", "");
                if (parseInstalledVersion(version) == null || version.contains("-")) {
                    return null;
                }
                String download = o.optString("download", "");
                String page = o.optString("page", "");
                if (!isAllowedUrl(download) || !isAllowedUrl(page)) {
                    return null;
                }
                String apk = o.isNull("apk") ? null : o.optString("apk", null);
                String sums = o.isNull("sums") ? null : o.optString("sums", null);
                String sig = o.isNull("sig") ? null : o.optString("sig", null);
                return new Result(Status.UPDATE_AVAILABLE, version, o.optString("notes", ""), download, page, null, apk, sums, sig);
            } catch (Exception e) {
                return null;
            }
        }
    }

    // A parsed and validated release, independent of which server it came from
    static final class Release {
        final String version;
        final String notes;
        final String pageUrl;
        final List<String[]> assets = new ArrayList<>(); // {name, url}

        Release(String version, String notes, String pageUrl) {
            this.version = version;
            this.notes = notes;
            this.pageUrl = pageUrl;
        }
    }

    private static final class FetchException extends Exception {
        final Failure failure;

        FetchException(Failure failure, String message) {
            super(message);
            this.failure = failure;
        }
    }

    private final OkHttpClient client;
    private final String githubUrl;
    private final String gitlabUrl;
    private final String installedVersion;
    private final List<String> abis;
    private final String userAgent;

    public UpdateChecker(String installedVersion, List<String> abis) {
        this(newClient(), TEST_BASE.isEmpty() ? GITHUB_LATEST_URL : TEST_BASE + "/latest.json",
                TEST_BASE.isEmpty() ? GITLAB_LATEST_URL : TEST_BASE + "/latest.json", installedVersion, abis);
    }

    UpdateChecker(OkHttpClient client, String githubUrl, String gitlabUrl, String installedVersion, List<String> abis) {
        this.client = client;
        this.githubUrl = githubUrl;
        this.gitlabUrl = gitlabUrl;
        this.installedVersion = installedVersion;
        this.abis = abis;
        this.userAgent = "NyxPlay-Android/" + (installedVersion == null ? "unknown" : installedVersion);
    }

    static OkHttpClient newClient() {
        // Platform default trust and hostname verification; a redirect from https to http is refused
        return new OkHttpClient.Builder()
                .connectTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .readTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .writeTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .callTimeout(3 * TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .followSslRedirects(false)
                .build();
    }

    public Result check() {
        try {
            int[] installed = parseInstalledVersion(installedVersion);
            if (installed == null) {
                // Development or unknown builds are never offered an update
                return Result.upToDate(installedVersion);
            }

            Release release = null;
            boolean sawServerProblem = false;
            String[] sources = {githubUrl, gitlabUrl};
            for (int i = 0; i < sources.length; i++) {
                try {
                    String body = fetch(sources[i]);
                    release = (i == 0) ? parseGitHub(body) : parseGitLab(body);
                    if (release != null) {
                        break;
                    }
                    sawServerProblem = true;
                    Log.d(TAG, "Source " + (i + 1) + " returned no usable release");
                } catch (FetchException e) {
                    if (e.failure == Failure.BAD_RESPONSE) {
                        sawServerProblem = true;
                    }
                    Log.d(TAG, "Source " + (i + 1) + " failed: " + e.getMessage());
                }
            }

            if (release == null) {
                return Result.failed(sawServerProblem ? Failure.BAD_RESPONSE : Failure.NO_CONNECTION);
            }

            int[] latest = parseTag(release.version);
            if (latest == null || compareVersions(latest, installed) <= 0) {
                return Result.upToDate(displayVersion(release.version));
            }

            String pageUrl = isAllowedUrl(release.pageUrl) ? release.pageUrl : RELEASES_PAGE_URL;
            String[] apk = selectApkAsset(release.assets, abis);
            String sums = findAssetUrl(release.assets, SUMS_NAME);
            String sig = findAssetUrl(release.assets, SIG_NAME);
            return Result.update(displayVersion(release.version), releaseNotesExcerpt(release.notes, MAX_NOTES_CHARS),
                    apk != null ? apk[1] : pageUrl, pageUrl,
                    apk != null ? apk[0] : null, apk != null ? sums : null, apk != null ? sig : null);
        } catch (Throwable t) {
            Log.d(TAG, "Check failed unexpectedly: " + t.getClass().getSimpleName());
            return Result.failed(Failure.BAD_RESPONSE);
        }
    }

    private String fetch(String url) throws FetchException {
        try {
            Request request = new Request.Builder()
                    .url(url)
                    .header("User-Agent", userAgent)
                    .header("Accept", "application/json")
                    .build();
            try (Response response = client.newCall(request).execute()) {
                if (!response.isSuccessful()) {
                    // Includes rate limits (403/429) and missing or private projects (404/401)
                    throw new FetchException(Failure.BAD_RESPONSE, "HTTP " + response.code());
                }
                ResponseBody body = response.body();
                if (body == null) {
                    throw new FetchException(Failure.BAD_RESPONSE, "empty body");
                }
                if (body.contentLength() > MAX_BODY_BYTES) {
                    throw new FetchException(Failure.BAD_RESPONSE, "body too large");
                }
                BufferedSource source = body.source();
                source.request(MAX_BODY_BYTES + 1L);
                if (source.getBuffer().size() > MAX_BODY_BYTES) {
                    throw new FetchException(Failure.BAD_RESPONSE, "body too large");
                }
                return source.readString(StandardCharsets.UTF_8);
            }
        } catch (FetchException e) {
            throw e;
        } catch (IOException e) {
            throw new FetchException(Failure.NO_CONNECTION, e.getClass().getSimpleName());
        } catch (RuntimeException e) {
            throw new FetchException(Failure.BAD_RESPONSE, e.getClass().getSimpleName());
        }
    }

    // ---- Parsing ----

    static Release parseGitHub(String json) {
        try {
            JSONObject root = new JSONObject(json);
            if (root.optBoolean("draft", false) || root.optBoolean("prerelease", false)) {
                return null;
            }
            String tag = root.optString("tag_name", "");
            if (!isReleaseTag(tag)) {
                return null;
            }
            String body = root.isNull("body") ? "" : root.optString("body", "");
            Release release = new Release(tag, body, root.optString("html_url", ""));
            JSONArray assets = root.optJSONArray("assets");
            if (assets != null) {
                for (int i = 0; i < assets.length(); i++) {
                    JSONObject asset = assets.optJSONObject(i);
                    if (asset != null) {
                        release.assets.add(new String[] {asset.optString("name", ""), asset.optString("browser_download_url", "")});
                    }
                }
            }
            return release;
        } catch (Exception e) {
            return null;
        }
    }

    static Release parseGitLab(String json) {
        try {
            JSONObject root = new JSONObject(json);
            if (root.optBoolean("upcoming_release", false)) {
                return null;
            }
            String tag = root.optString("tag_name", "");
            if (!isReleaseTag(tag)) {
                return null;
            }
            String body = root.isNull("description") ? "" : root.optString("description", "");
            JSONObject links = root.optJSONObject("_links");
            Release release = new Release(tag, body, links == null ? "" : links.optString("self", ""));
            JSONObject assets = root.optJSONObject("assets");
            JSONArray list = assets == null ? null : assets.optJSONArray("links");
            if (list != null) {
                for (int i = 0; i < list.length(); i++) {
                    JSONObject asset = list.optJSONObject(i);
                    if (asset != null) {
                        release.assets.add(new String[] {asset.optString("name", ""), asset.optString("url", "")});
                    }
                }
            }
            return release;
        } catch (Exception e) {
            return null;
        }
    }

    // ---- Pure helpers ----

    /** True for calendar release tags such as v2026.11.19.1 (host-v..., rc tags and the like are rejected). */
    public static boolean isReleaseTag(String tag) {
        return parseTag(tag) != null;
    }

    static int[] parseTag(String tag) {
        if (tag == null) {
            return null;
        }
        Matcher m = TAG_PATTERN.matcher(tag);
        if (!m.matches()) {
            return null;
        }
        return new int[] {Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2)),
                Integer.parseInt(m.group(3)), Integer.parseInt(m.group(4))};
    }

    /** Numeric part of a versionName; any suffix such as "-debug" is ignored. Null when unusable. */
    public static int[] parseInstalledVersion(String versionName) {
        if (versionName == null) {
            return null;
        }
        String v = versionName.trim();
        int dash = v.indexOf('-');
        if (dash >= 0) {
            v = v.substring(0, dash);
        }
        Matcher m = VERSION_PREFIX.matcher(v);
        if (!m.matches()) {
            return null;
        }
        String[] parts = v.split("\\.");
        int[] out = new int[parts.length];
        for (int i = 0; i < parts.length; i++) {
            out[i] = Integer.parseInt(parts[i]);
        }
        return out;
    }

    /** Component-wise numeric comparison; missing trailing components count as 0. */
    public static int compareVersions(int[] a, int[] b) {
        int n = Math.max(a.length, b.length);
        for (int i = 0; i < n; i++) {
            int x = i < a.length ? a[i] : 0;
            int y = i < b.length ? b[i] : 0;
            if (x != y) {
                return x < y ? -1 : 1;
            }
        }
        return 0;
    }

    /** Version string shown to the user: the tag without a leading "v". */
    public static String displayVersion(String tag) {
        return tag != null && tag.startsWith("v") ? tag.substring(1) : tag;
    }

    /**
     * Picks the APK for the first ABI (in the device's preference order) that has one, or null.
     * Only assets with an allowed download URL are considered.
     */
    public static String selectApkUrl(List<String[]> assets, List<String> abis) {
        String[] asset = selectApkAsset(assets, abis);
        return asset == null ? null : asset[1];
    }

    /** Like selectApkUrl but returns {name, url}. */
    public static String[] selectApkAsset(List<String[]> assets, List<String> abis) {
        if (assets == null || abis == null) {
            return null;
        }
        for (String abi : abis) {
            if (abi == null || abi.isEmpty()) {
                continue;
            }
            String needle = "-" + abi.toLowerCase(Locale.ROOT) + "-";
            for (String[] asset : assets) {
                String name = asset[0].toLowerCase(Locale.ROOT);
                if (name.endsWith(".apk") && name.contains(needle) && isAllowedUrl(asset[1]) && isSafeAssetName(asset[0])) {
                    return asset;
                }
            }
        }
        return null;
    }

    /** URL of the asset with exactly this name, if it is an allowed address. */
    static String findAssetUrl(List<String[]> assets, String name) {
        for (String[] asset : assets) {
            if (asset[0].equals(name) && isAllowedUrl(asset[1])) {
                return asset[1];
            }
        }
        return null;
    }

    /** A plain file name: no separators or control characters, so it cannot name another path. */
    static boolean isSafeAssetName(String name) {
        return name != null && name.length() > 0 && name.length() <= 128 && name.matches("[A-Za-z0-9._+-]+")
                && !name.contains("..");
    }

    private static boolean isTestUrl(String url) {
        return !TEST_BASE.isEmpty() && url.startsWith(TEST_BASE + "/") && !url.contains("..");
    }

    /** Only the project's own pages and GitHub's release asset hosts may be opened. */
    public static boolean isAllowedUrl(String url) {
        if (url == null || url.isEmpty() || url.length() > 2048) {
            return false;
        }
        if (isTestUrl(url)) {
            return true;
        }
        try {
            URI uri = new URI(url).normalize();
            if (!"https".equals(uri.getScheme()) || uri.getRawUserInfo() != null
                    || uri.getPort() != -1 || uri.getHost() == null) {
                return false;
            }
            String host = uri.getHost().toLowerCase(Locale.ROOT);
            String path = uri.getPath() == null ? "" : uri.getPath();
            if (path.contains("..")) {
                return false;
            }
            if (host.equals("objects.githubusercontent.com") || host.equals("release-assets.githubusercontent.com")) {
                return true;
            }
            if (host.equals("github.com") || host.equals("gitlab.com")) {
                String lower = path.toLowerCase(Locale.ROOT);
                return lower.equals("/siberanka/nyxplay") || lower.startsWith("/siberanka/nyxplay/");
            }
            return false;
        } catch (Exception e) {
            return false;
        }
    }

    /** Plain-text excerpt of Markdown release notes; markup is removed and nothing is rendered as HTML. */
    public static String releaseNotesExcerpt(String markdown, int maxChars) {
        if (markdown == null) {
            return "";
        }
        StringBuilder sb = new StringBuilder();
        boolean inFence = false;
        for (String rawLine : markdown.split("\r?\n")) {
            String line = rawLine.trim();
            if (line.startsWith("```") || line.startsWith("~~~")) {
                inFence = !inFence;
                continue;
            }
            // Headings and tables carry titles and download lists, not the summary
            if (inFence || line.isEmpty() || line.startsWith("#") || line.startsWith("|")) {
                continue;
            }
            line = line.replaceAll("<[^>]*>", "");
            line = line.replaceAll("!\\[([^\\]]*)\\]\\([^)]*\\)", "$1");
            line = line.replaceAll("\\[([^\\]]*)\\]\\([^)]*\\)", "$1");
            line = line.replaceAll("^(>\\s*)+", "");
            line = line.replaceAll("^([-*+]|\\d+[.)])\\s+", "- ");
            line = line.replaceAll("[*`~]+", "");
            line = line.replaceAll("(?<![A-Za-z0-9])_+|_+(?![A-Za-z0-9])", "");
            line = line.replaceAll("\\p{Cntrl}", "").trim();
            if (line.isEmpty()) {
                continue;
            }
            if (sb.length() > 0) {
                sb.append('\n');
            }
            sb.append(line);
            if (sb.length() > maxChars * 2) {
                break;
            }
        }
        String text = sb.toString();
        if (text.length() <= maxChars) {
            return text;
        }
        int cut = text.lastIndexOf(' ', maxChars);
        if (cut < maxChars / 2) {
            cut = maxChars;
        }
        return text.substring(0, cut).trim() + "\u2026";
    }
}
