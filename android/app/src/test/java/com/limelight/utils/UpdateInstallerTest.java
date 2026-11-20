package com.limelight.utils;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.TestLogSuppressor;

import org.junit.After;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import okhttp3.HttpUrl;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.mockwebserver.Dispatcher;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.mockwebserver.RecordedRequest;
import okio.Buffer;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class UpdateInstallerTest {
    private static final String VERSION = "2026.12.1.1";
    private static final String APK_NAME = "NyxPlay-" + VERSION + "-x86_64-release.apk";
    private static final String BASE = "https://github.com/siberanka/NyxPlay/releases/download/v" + VERSION + "/";
    private static final String PAGE = "https://github.com/siberanka/NyxPlay/releases/tag/v" + VERSION;

    private MockWebServer server;
    private final Map<String, MockResponse> routes = new ConcurrentHashMap<>();
    private final List<String> requested = new CopyOnWriteArrayList<>();
    private Context context;
    private byte[] apkBytes;
    private byte[] sums;
    private byte[] sig;
    private FakeBackend backend;
    private final List<UpdateInstaller.State> seen = new ArrayList<>();

    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    static final class FakeBackend implements UpdateInstaller.Backend {
        int installs;
        int abandons;
        byte[] installedBytes;
        boolean failWithIo;

        @Override
        public int install(Context context, File apk) throws IOException {
            installs++;
            if (failWithIo) {
                throw new IOException("write failed: ENOSPC (No space left on device)");
            }
            installedBytes = readAll(apk);
            UpdateInstaller.sessionCreated(7);
            return 7;
        }

        @Override
        public void abandon(Context context, int sessionId) {
            abandons++;
        }
    }

    @Before
    public void setUp() throws Exception {
        context = ApplicationProvider.getApplicationContext();
        apkBytes = resource("/release/update/" + APK_NAME);
        sums = resource("/release/update/SHA256SUMS.txt");
        sig = resource("/release/update/SHA256SUMS.txt.sig");

        server = new MockWebServer();
        server.setDispatcher(new Dispatcher() {
            @Override
            public MockResponse dispatch(RecordedRequest request) {
                requested.add(request.getPath());
                MockResponse r = routes.get(request.getPath());
                return r != null ? r : new MockResponse().setResponseCode(404);
            }
        });
        server.start();
        serveRelease(apkBytes, sums, sig);

        backend = new FakeBackend();
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        }, redirectingClient(), backend, "2026.11.19.1");
        UpdateInstaller.addListener(new UpdateInstaller.Listener() {
            @Override
            public void onUpdateState(UpdateInstaller.State state) {
                seen.add(state);
            }
        });
        context.getSharedPreferences("update_check", Context.MODE_PRIVATE).edit().clear().commit();
    }

    @After
    public void tearDown() throws Exception {
        UpdateInstaller.setTestHooks(null, null, null, null);
        server.shutdown();
    }

    // ---- helpers ----

    private OkHttpClient redirectingClient() {
        // The code under test only talks to the allowed release hosts; the test sends those requests to the local server
        return new OkHttpClient.Builder()
                .followRedirects(false)
                .addInterceptor(new okhttp3.Interceptor() {
                    @Override
                    public okhttp3.Response intercept(Chain chain) throws IOException {
                        Request r = chain.request();
                        HttpUrl local = r.url().newBuilder().scheme("http").host(server.getHostName()).port(server.getPort()).build();
                        return chain.proceed(r.newBuilder().url(local).build());
                    }
                })
                .build();
    }

    private void serveRelease(byte[] apk, byte[] sumsBytes, byte[] sigBytes) {
        routes.put(path(APK_NAME), body(apk));
        routes.put(path("SHA256SUMS.txt"), body(sumsBytes));
        routes.put(path("SHA256SUMS.txt.sig"), body(sigBytes));
    }

    private static String path(String name) {
        return "/siberanka/NyxPlay/releases/download/v" + VERSION + "/" + name;
    }

    private static MockResponse body(byte[] data) {
        return new MockResponse().setBody(new Buffer().write(data));
    }

    private UpdateChecker.Result offer() {
        return UpdateChecker.Result.update(VERSION, "", BASE + APK_NAME, PAGE, APK_NAME, BASE + "SHA256SUMS.txt", BASE + "SHA256SUMS.txt.sig");
    }

    private void idle() {
        Shadows.shadowOf(android.os.Looper.getMainLooper()).idle();
    }

    private List<UpdateInstaller.Phase> phases() {
        idle();
        List<UpdateInstaller.Phase> out = new ArrayList<>();
        for (UpdateInstaller.State s : seen) {
            out.add(s.phase);
        }
        return out;
    }

    private static byte[] readAll(File f) throws IOException {
        try (InputStream in = new java.io.FileInputStream(f)) {
            return readAll(in);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) >= 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private byte[] resource(String name) throws IOException {
        try (InputStream in = getClass().getResourceAsStream(name)) {
            assertNotNull(name, in);
            return readAll(in);
        }
    }

    private File[] cacheFiles() {
        File[] files = UpdateInstaller.updateDir(context).listFiles();
        return files == null ? new File[0] : files;
    }

    private int apkRequests() {
        int n = 0;
        for (String p : requested) {
            if (p.endsWith(".apk")) {
                n++;
            }
        }
        return n;
    }

    // ---- State machine ----

    @Test
    public void happyPathRunsIdleDownloadingVerifyingInstalling() {
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertTrue(UpdateInstaller.start(context, offer()));

        List<UpdateInstaller.Phase> phases = phases();
        // sums + signature, signature check, apk download, hash check, install session
        int firstDownload = phases.indexOf(UpdateInstaller.Phase.DOWNLOADING);
        int firstVerify = phases.indexOf(UpdateInstaller.Phase.VERIFYING);
        int install = phases.indexOf(UpdateInstaller.Phase.INSTALLING);
        assertTrue(phases.toString(), firstDownload == 0 && firstVerify > firstDownload && install > firstVerify);
        assertEquals(UpdateInstaller.Phase.INSTALLING, UpdateInstaller.getState().phase);
        assertEquals(1, backend.installs);
        assertArrayEquals(apkBytes, backend.installedBytes);
        // the download never stays in the cache
        assertEquals(0, cacheFiles().length);
        // 100 percent was reported while downloading
        boolean sawFull = false;
        for (UpdateInstaller.State s : seen) {
            sawFull |= s.phase == UpdateInstaller.Phase.DOWNLOADING && s.percent == 100;
        }
        assertTrue(sawFull);
        // the update marker for the "Updated to" notice is written when the session is committed
        assertEquals(VERSION, context.getSharedPreferences("update_check", Context.MODE_PRIVATE)
                .getString(UpdateInstaller.KEY_INSTALLING_VERSION, ""));
    }

    @Test
    public void staleConnectionFailureIsRetriedOnceOnAFreshConnection() {
        final AtomicInteger calls = new AtomicInteger();
        OkHttpClient flaky = redirectingClient().newBuilder()
                .addInterceptor(new okhttp3.Interceptor() {
                    @Override
                    public okhttp3.Response intercept(Chain chain) throws IOException {
                        if (calls.getAndIncrement() == 0) {
                            throw new java.net.SocketException("Connection reset");
                        }
                        return chain.proceed(chain.request());
                    }
                })
                .build();
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        }, flaky, backend, "2026.11.19.1");
        assertTrue(UpdateInstaller.start(context, offer()));
        assertEquals(UpdateInstaller.Phase.INSTALLING, UpdateInstaller.getState().phase);
        assertEquals(1, backend.installs);
    }

    @Test
    public void persistentConnectionFailureEndsAsNoConnection() {
        OkHttpClient broken = redirectingClient().newBuilder()
                .addInterceptor(new okhttp3.Interceptor() {
                    @Override
                    public okhttp3.Response intercept(Chain chain) throws IOException {
                        throw new java.net.SocketException("Connection reset");
                    }
                })
                .build();
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        }, broken, backend, "2026.11.19.1");
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Phase.FAILED, UpdateInstaller.getState().phase);
        assertEquals(UpdateInstaller.Failure.NO_CONNECTION, UpdateInstaller.getState().failure);
        assertEquals(0, backend.installs);
    }

    @Test
    public void signatureIsCheckedBeforeTheApkIsRequested() {
        assertTrue(UpdateInstaller.start(context, offer()));
        List<String> order = new ArrayList<>(requested);
        assertEquals(3, order.size());
        assertTrue(order.get(2).endsWith(".apk"));
    }

    @Test
    public void secondStartWhileRunningIsRejected() {
        final Runnable[] queued = new Runnable[1];
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                queued[0] = command;
            }
        }, redirectingClient(), backend, "2026.11.19.1");
        assertTrue(UpdateInstaller.start(context, offer()));
        assertFalse(UpdateInstaller.start(context, offer()));
        assertEquals(UpdateInstaller.Phase.DOWNLOADING, UpdateInstaller.getState().phase);
    }

    @Test
    public void notNewerOrNotInstallableOffersAreRefused() {
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        }, redirectingClient(), backend, VERSION + "-debug");
        assertFalse(UpdateInstaller.start(context, offer()));
        assertEquals(UpdateInstaller.Failure.UNSUPPORTED, UpdateInstaller.getState().failure);
        assertEquals(0, requested.size());

        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        }, redirectingClient(), backend, "2026.11.19.1");
        // no signed list in the release: the app must not install it
        UpdateChecker.Result noSums = UpdateChecker.Result.update(VERSION, "", BASE + APK_NAME, PAGE);
        assertFalse(UpdateInstaller.start(context, noSums));
        // an asset name without the offered version is refused
        UpdateChecker.Result wrongName = UpdateChecker.Result.update(VERSION, "", BASE + "NyxPlay-2026.1.1.1-x86_64-release.apk", PAGE,
                "NyxPlay-2026.1.1.1-x86_64-release.apk", BASE + "SHA256SUMS.txt", BASE + "SHA256SUMS.txt.sig");
        assertFalse(UpdateInstaller.start(context, wrongName));
        assertEquals(0, requested.size());
    }

    // ---- Verification failures delete the file ----

    @Test
    public void tamperedApkIsRejectedAndDeleted() {
        byte[] bad = apkBytes.clone();
        bad[100] ^= 1;
        routes.put(path(APK_NAME), body(bad));

        assertTrue(UpdateInstaller.start(context, offer()));
        assertEquals(UpdateInstaller.Phase.FAILED, UpdateInstaller.getState().phase);
        assertEquals(UpdateInstaller.Failure.NOT_VERIFIED, UpdateInstaller.getState().failure);
        assertEquals(0, backend.installs);
        assertEquals(0, cacheFiles().length);
        assertEquals("", context.getSharedPreferences("update_check", Context.MODE_PRIVATE)
                .getString(UpdateInstaller.KEY_INSTALLING_VERSION, ""));
    }

    @Test
    public void tamperedSignatureIsRejectedBeforeTheApkIsDownloaded() {
        byte[] bad = sig.clone();
        bad[10] = (byte) (bad[10] == 'A' ? 'B' : 'A');
        routes.put(path("SHA256SUMS.txt.sig"), body(bad));

        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.NOT_VERIFIED, UpdateInstaller.getState().failure);
        assertEquals(0, apkRequests());
        assertEquals(0, backend.installs);
        assertEquals(0, cacheFiles().length);
    }

    @Test
    public void tamperedChecksumListIsRejected() {
        byte[] bad = sums.clone();
        bad[0] = (byte) (bad[0] == '3' ? '4' : '3');
        routes.put(path("SHA256SUMS.txt"), body(bad));

        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.NOT_VERIFIED, UpdateInstaller.getState().failure);
        assertEquals(0, apkRequests());
    }

    @Test
    public void malformedSignatureIsRejected() {
        routes.put(path("SHA256SUMS.txt.sig"), body("not a signature\n".getBytes()));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.NOT_VERIFIED, UpdateInstaller.getState().failure);
        assertEquals(0, apkRequests());
    }

    @Test
    public void assetMissingFromTheSignedListIsRejected() {
        String other = "NyxPlay-" + VERSION + "-arm64-v8a-release.apk";
        routes.put(path(other), body(apkBytes));
        UpdateChecker.Result o = UpdateChecker.Result.update(VERSION, "", BASE + other, PAGE, other,
                BASE + "SHA256SUMS.txt", BASE + "SHA256SUMS.txt.sig");
        UpdateInstaller.start(context, o);
        assertEquals(UpdateInstaller.Failure.NOT_VERIFIED, UpdateInstaller.getState().failure);
        assertEquals(0, apkRequests());
    }

    // ---- Download problems ----

    @Test
    public void notFoundAndServerErrorsFailTheDownload() {
        routes.remove(path(APK_NAME));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.DOWNLOAD, UpdateInstaller.getState().failure);
        assertEquals(0, cacheFiles().length);
    }

    @Test
    public void shortBodyIsRejectedAndDeleted() {
        // Content-Length says 20000 bytes but the connection ends early
        routes.put(path(APK_NAME), body(apkBytes).setSocketPolicy(okhttp3.mockwebserver.SocketPolicy.DISCONNECT_DURING_RESPONSE_BODY));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Phase.FAILED, UpdateInstaller.getState().phase);
        assertEquals(0, backend.installs);
        assertEquals(0, cacheFiles().length);
    }

    @Test
    public void redirectToAForeignHostIsRefused() {
        routes.put(path(APK_NAME), new MockResponse().setResponseCode(302).setHeader("Location", "https://evil.example/a.apk"));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.DOWNLOAD, UpdateInstaller.getState().failure);
        assertEquals(1, apkRequests());
        assertEquals(0, backend.installs);
    }

    @Test
    public void redirectToHttpIsRefused() {
        routes.put(path(APK_NAME), new MockResponse().setResponseCode(302)
                .setHeader("Location", "http://objects.githubusercontent.com/a.apk"));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.DOWNLOAD, UpdateInstaller.getState().failure);
    }

    @Test
    public void redirectToTheGithubAssetHostIsFollowedAndTooManyAreNot() {
        // One hop to the asset host works
        routes.put(path(APK_NAME), new MockResponse().setResponseCode(302)
                .setHeader("Location", "https://objects.githubusercontent.com/blob/asset"));
        routes.put("/blob/asset", body(apkBytes));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Phase.INSTALLING, UpdateInstaller.getState().phase);

        // Six hops are too many
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                command.run();
            }
        }, redirectingClient(), backend, "2026.11.19.1");
        for (int i = 0; i < 6; i++) {
            routes.put(i == 0 ? path(APK_NAME) : "/hop" + i,
                    new MockResponse().setResponseCode(302).setHeader("Location", "https://objects.githubusercontent.com/hop" + (i + 1)));
        }
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.DOWNLOAD, UpdateInstaller.getState().failure);
    }

    @Test
    public void unreachableServerIsReportedAsNoConnection() throws Exception {
        server.shutdown();
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.NO_CONNECTION, UpdateInstaller.getState().failure);
        assertEquals(0, cacheFiles().length);
    }

    @Test
    public void oversizedSignatureOrListIsRejected() {
        routes.put(path("SHA256SUMS.txt.sig"), body(new byte[UpdateInstaller.MAX_SIG_BYTES + 1]));
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.DOWNLOAD, UpdateInstaller.getState().failure);
    }

    @Test
    public void installSessionDiskFullIsReportedAsStorage() {
        backend.failWithIo = true;
        UpdateInstaller.start(context, offer());
        assertEquals(UpdateInstaller.Failure.STORAGE, UpdateInstaller.getState().failure);
        assertEquals(0, cacheFiles().length);
    }

    // ---- Cancel ----

    @Test
    public void cancelDuringDownloadStopsAndCleans() throws Exception {
        routes.put(path(APK_NAME), body(apkBytes).throttleBody(2000, 200, TimeUnit.MILLISECONDS));
        final Thread[] worker = new Thread[1];
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                worker[0] = new Thread(command);
                worker[0].start();
            }
        }, redirectingClient(), backend, "2026.11.19.1");

        assertTrue(UpdateInstaller.start(context, offer()));
        long deadline = System.currentTimeMillis() + 10000;
        while (System.currentTimeMillis() < deadline
                && !(UpdateInstaller.getState().phase == UpdateInstaller.Phase.DOWNLOADING && UpdateInstaller.getState().percent > 0)) {
            Thread.sleep(20);
        }
        assertTrue(UpdateInstaller.getState().percent > 0);

        UpdateInstaller.cancel(context);
        worker[0].join(10000);
        assertFalse(worker[0].isAlive());

        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertEquals(0, backend.installs);
        assertEquals(0, cacheFiles().length);
    }

    @Test
    public void cancelBeforeTheWorkerStartsEndsIdle() {
        final Runnable[] queued = new Runnable[1];
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                queued[0] = command;
            }
        }, redirectingClient(), backend, "2026.11.19.1");
        assertTrue(UpdateInstaller.start(context, offer()));
        UpdateInstaller.cancel(context);
        queued[0].run();
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertEquals(0, requested.size());
        assertEquals(0, backend.installs);
    }

    @Test
    public void cancelWhileWaitingForConfirmationAbandonsTheSession() {
        UpdateInstaller.start(context, offer());
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_PENDING_USER_ACTION, 7, new Intent("confirm"));
        assertTrue(UpdateInstaller.getState().needsConfirmation);

        UpdateInstaller.cancel(context);
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
        assertEquals(1, backend.abandons);
        assertNull(UpdateInstaller.takeConfirmation());
    }

    // ---- Install results ----

    @Test
    public void confirmationIsHandedOverOnce() {
        UpdateInstaller.start(context, offer());
        Intent confirm = new Intent("confirm");
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_PENDING_USER_ACTION, 7, confirm);
        assertTrue(UpdateInstaller.getState().needsConfirmation);
        assertSame(confirm, UpdateInstaller.takeConfirmation());
        assertFalse(UpdateInstaller.getState().needsConfirmation);
        assertNull(UpdateInstaller.takeConfirmation());
    }

    @Test
    public void statusOfAnotherSessionIsIgnored() {
        UpdateInstaller.start(context, offer());
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_FAILURE, 99, null);
        assertEquals(UpdateInstaller.Phase.INSTALLING, UpdateInstaller.getState().phase);
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_PENDING_USER_ACTION, 99, new Intent("x"));
        assertFalse(UpdateInstaller.getState().needsConfirmation);
    }

    @Test
    public void statusWithoutARunIsIgnored() {
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_FAILURE, 7, null);
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
    }

    @Test
    public void failureStatusesMapToClearReasons() {
        assertEquals(UpdateInstaller.Failure.BLOCKED, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE_BLOCKED));
        assertEquals(UpdateInstaller.Failure.CONFLICT, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE_CONFLICT));
        assertEquals(UpdateInstaller.Failure.STORAGE, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE_STORAGE));
        assertEquals(UpdateInstaller.Failure.INCOMPATIBLE, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE_INCOMPATIBLE));
        assertEquals(UpdateInstaller.Failure.ABORTED, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE_ABORTED));
        assertEquals(UpdateInstaller.Failure.OTHER, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE));
        assertEquals(UpdateInstaller.Failure.OTHER, UpdateInstaller.failureForStatus(PackageInstaller.STATUS_FAILURE_INVALID));
        for (UpdateInstaller.Failure f : UpdateInstaller.Failure.values()) {
            assertTrue(f.name(), UpdateProgressUi.failureMessageRes(f) != 0);
        }
    }

    @Test
    public void installFailureStatusEndsInFailedAndClearsTheMarker() {
        UpdateInstaller.start(context, offer());
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_FAILURE_CONFLICT, 7, null);
        assertEquals(UpdateInstaller.Phase.FAILED, UpdateInstaller.getState().phase);
        assertEquals(UpdateInstaller.Failure.CONFLICT, UpdateInstaller.getState().failure);
        assertEquals("", context.getSharedPreferences("update_check", Context.MODE_PRIVATE)
                .getString(UpdateInstaller.KEY_INSTALLING_VERSION, ""));
        UpdateInstaller.acknowledge();
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
    }

    @Test
    public void successStatusReturnsToIdle() {
        UpdateInstaller.start(context, offer());
        UpdateInstaller.onInstallStatus(context, PackageInstaller.STATUS_SUCCESS, 7, null);
        assertEquals(UpdateInstaller.Phase.IDLE, UpdateInstaller.getState().phase);
    }

    @Test
    public void listenersAreCalledOnTheMainThreadInOrder() {
        UpdateInstaller.start(context, offer());
        assertTrue("posted, not called inline", seen.isEmpty());
        idle();
        assertFalse(seen.isEmpty());
        assertEquals(UpdateInstaller.Phase.DOWNLOADING, seen.get(0).phase);
    }

    // ---- Leftovers and "Updated to" ----

    @Test
    public void leftoversAreRemovedWhenIdleButNotWhileRunning() throws Exception {
        File dir = UpdateInstaller.updateDir(context);
        assertTrue(dir.isDirectory() || dir.mkdirs());
        File stale = new File(dir, "update.apk.part");
        try (FileOutputStream out = new FileOutputStream(stale)) {
            out.write(1);
        }
        UpdateInstaller.cleanupLeftovers(context);
        assertFalse(stale.exists());

        // While a run is active its files are left alone
        final Runnable[] queued = new Runnable[1];
        UpdateInstaller.setTestHooks(new Executor() {
            @Override
            public void execute(Runnable command) {
                queued[0] = command;
            }
        }, redirectingClient(), backend, "2026.11.19.1");
        UpdateInstaller.start(context, offer());
        try (FileOutputStream out = new FileOutputStream(stale)) {
            out.write(1);
        }
        UpdateInstaller.cleanupLeftovers(context);
        assertTrue(stale.exists());
    }

    private void marker(String version, long ageMs) {
        context.getSharedPreferences("update_check", Context.MODE_PRIVATE).edit()
                .putString(UpdateInstaller.KEY_INSTALLING_VERSION, version)
                .putLong(UpdateInstaller.KEY_INSTALLING_AT, System.currentTimeMillis() - ageMs).commit();
    }

    @Test
    public void updatedVersionIsReportedOnceWhenTheRunningVersionReachedIt() {
        UpdateInstaller.setTestHooks(null, null, null, "2026.12.1.1-debug");
        marker("2026.12.1.1", 5000);
        assertEquals("2026.12.1.1", UpdateInstaller.consumeUpdatedVersion(context));
        assertNull(UpdateInstaller.consumeUpdatedVersion(context));
    }

    @Test
    public void newerRunningVersionAlsoCounts() {
        UpdateInstaller.setTestHooks(null, null, null, "2027.1.1.1");
        marker("2026.12.1.1", 5000);
        assertEquals("2026.12.1.1", UpdateInstaller.consumeUpdatedVersion(context));
    }

    @Test
    public void markerOfAnInstallThatDidNotHappenIsNotReportedAndExpires() {
        UpdateInstaller.setTestHooks(null, null, null, "2026.11.19.1");
        marker("2026.12.1.1", 5000);
        assertNull(UpdateInstaller.consumeUpdatedVersion(context));
        SharedPreferences prefs = context.getSharedPreferences("update_check", Context.MODE_PRIVATE);
        assertEquals("2026.12.1.1", prefs.getString(UpdateInstaller.KEY_INSTALLING_VERSION, ""));

        marker("2026.12.1.1", UpdateInstaller.MARKER_MAX_AGE_MS + 1000);
        assertNull(UpdateInstaller.consumeUpdatedVersion(context));
        assertEquals("", prefs.getString(UpdateInstaller.KEY_INSTALLING_VERSION, ""));
    }

    @Test
    public void garbageMarkerIsHarmless() {
        marker("not a version", 1000);
        assertNull(UpdateInstaller.consumeUpdatedVersion(context));
    }

    @Test
    public void newerThanInstalledComparison() {
        assertTrue(UpdateInstaller.isNewerThanInstalled("2026.12.1.1"));
        assertFalse(UpdateInstaller.isNewerThanInstalled("2026.11.19.1"));
        assertFalse(UpdateInstaller.isNewerThanInstalled("2026.11.1.1"));
        assertFalse(UpdateInstaller.isNewerThanInstalled("junk"));
        assertEquals(Arrays.asList(1), Arrays.asList(1));
    }
}
