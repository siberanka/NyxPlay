package com.limelight.utils;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.limelight.BuildConfig;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;

import okhttp3.Call;
import okhttp3.HttpUrl;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.ResponseBody;

/**
 * Downloads a release APK, checks it against the signed checksum list and hands it to the system
 * package installer. The work lives in the process, not in an activity, so it survives rotation and
 * (with UpdateService) the user leaving the app.
 *
 * Order, as for every NyxPlay updater: signature of SHA256SUMS.txt, then the APK, then its SHA-256
 * against the signed list, and only then the install session. Any failure deletes the download.
 */
public final class UpdateInstaller {
    private static final String TAG = "NyxPlayUpdate";

    public enum Phase {
        IDLE,
        DOWNLOADING,
        VERIFYING,
        INSTALLING,
        FAILED
    }

    public enum Failure {
        NO_CONNECTION,
        DOWNLOAD,
        NOT_VERIFIED,
        STORAGE,
        BLOCKED,
        CONFLICT,
        INCOMPATIBLE,
        ABORTED,
        UNSUPPORTED,
        OTHER
    }

    /** Immutable snapshot of the install flow. */
    public static final class State {
        public final Phase phase;
        public final Failure failure;
        /** 0-100 while downloading the APK, -1 when not known. */
        public final int percent;
        public final String version;
        /** The system asks the user to confirm the install and the confirmation has not been shown yet. */
        public final boolean needsConfirmation;

        State(Phase phase, Failure failure, int percent, String version, boolean needsConfirmation) {
            this.phase = phase;
            this.failure = failure;
            this.percent = percent;
            this.version = version;
            this.needsConfirmation = needsConfirmation;
        }

        public boolean isActive() {
            return phase == Phase.DOWNLOADING || phase == Phase.VERIFYING || phase == Phase.INSTALLING;
        }
    }

    public interface Listener {
        /** Always called on the main thread. */
        void onUpdateState(State state);
    }

    /** Creates install sessions; replaced in tests. */
    interface Backend {
        /** Must call sessionCreated(id) before committing the session. */
        int install(Context context, File apk) throws IOException;

        void abandon(Context context, int sessionId);
    }

    public static final long MAX_APK_BYTES = 150L * 1024 * 1024;
    static final int MAX_SIG_BYTES = 1024;
    static final int MAX_REDIRECTS = 5;
    static final long STORAGE_MARGIN_BYTES = 16L * 1024 * 1024;
    static final long MARKER_MAX_AGE_MS = 60L * 60 * 1000;

    static final String KEY_INSTALLING_VERSION = "installing_version";
    static final String KEY_INSTALLING_AT = "installing_at";

    private static final State IDLE_STATE = new State(Phase.IDLE, null, -1, null, false);

    private static final CopyOnWriteArrayList<Listener> listeners = new CopyOnWriteArrayList<>();
    private static final Handler mainHandler = new Handler(Looper.getMainLooper());

    private static volatile State state = IDLE_STATE;
    private static volatile boolean cancelled;
    private static volatile Call currentCall;
    private static volatile int sessionId = -1;
    private static volatile Intent confirmIntent;
    private static ExecutorService defaultExecutor;
    private static Executor executor;
    private static OkHttpClient client;
    private static Backend backend;
    private static String versionName = BuildConfig.VERSION_NAME;

    private UpdateInstaller() {}

    // ---- Observers ----

    public static State getState() {
        return state;
    }

    public static void addListener(Listener listener) {
        listeners.addIfAbsent(listener);
    }

    public static void removeListener(Listener listener) {
        listeners.remove(listener);
    }

    private static void publish(State next) {
        state = next;
        final State snapshot = next;
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                for (Listener l : listeners) {
                    l.onUpdateState(snapshot);
                }
            }
        });
    }

    // ---- Control ----

    /**
     * Starts the download and install of an offered update. Returns false when another run is active,
     * when the offer cannot be verified in the app, or when it is not newer than the installed version.
     */
    public static synchronized boolean start(Context context, UpdateChecker.Result offer) {
        if (state.isActive()) {
            return false;
        }
        if (offer == null || !offer.canInstallInApp() || !isNewerThanInstalled(offer.version)) {
            publish(new State(Phase.FAILED, Failure.UNSUPPORTED, -1, offer == null ? null : offer.version, false));
            return false;
        }

        final Context app = context.getApplicationContext();
        final UpdateChecker.Result job = offer;
        cancelled = false;
        confirmIntent = null;
        sessionId = -1;
        publish(new State(Phase.DOWNLOADING, null, -1, offer.version, false));
        try {
            executor().execute(new Runnable() {
                @Override
                public void run() {
                    runJob(app, job);
                }
            });
        } catch (RuntimeException e) {
            publish(new State(Phase.FAILED, Failure.OTHER, -1, offer.version, false));
            return false;
        }
        return true;
    }

    /** Stops the download, or abandons an install session that is still waiting for confirmation. */
    public static void cancel(Context context) {
        State current = state;
        if (!current.isActive()) {
            return;
        }
        cancelled = true;
        Call call = currentCall;
        if (call != null) {
            call.cancel();
        }
        if (current.phase == Phase.INSTALLING) {
            int id = sessionId;
            sessionId = -1;
            confirmIntent = null;
            if (id >= 0) {
                backend().abandon(context.getApplicationContext(), id);
            }
            clearInstallingMarker(context);
            publish(IDLE_STATE);
        }
    }

    /** Removes a finished failure from the state so the next screen starts clean. */
    public static synchronized void acknowledge() {
        if (state.phase == Phase.FAILED) {
            publish(IDLE_STATE);
        }
    }

    /** The system's confirmation screen, handed over once; null when there is none to show. */
    public static synchronized Intent takeConfirmation() {
        Intent intent = confirmIntent;
        confirmIntent = null;
        State current = state;
        if (intent != null && current.phase == Phase.INSTALLING && current.needsConfirmation) {
            publish(new State(Phase.INSTALLING, null, -1, current.version, false));
        }
        return intent;
    }

    // ---- The worker ----

    private static final class Fail extends Exception {
        final Failure failure;

        Fail(Failure failure) {
            super(failure.name());
            this.failure = failure;
        }
    }

    private static void runJob(Context context, UpdateChecker.Result offer) {
        File dir = updateDir(context);
        File part = new File(dir, "update.apk.part");
        File apk = new File(dir, "update.apk");
        boolean committed = false;
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new Fail(Failure.STORAGE);
            }
            clearFiles(dir);
            abandonOwnSessions(context);

            byte[] signature = fetchBytes(offer.sigUrl, MAX_SIG_BYTES);
            byte[] sums = fetchBytes(offer.sumsUrl, ReleaseVerifier.MAX_SUMS_SIZE);

            publish(new State(Phase.VERIFYING, null, -1, offer.version, false));
            if (ReleaseVerifier.verifySignature(sums, signature) != ReleaseVerifier.Result.OK) {
                Log.w(TAG, "Release signature check failed");
                throw new Fail(Failure.NOT_VERIFIED);
            }
            if (ReleaseVerifier.findHash(sums, offer.apkName, new StringBuilder()) != ReleaseVerifier.Result.OK) {
                Log.w(TAG, "Release asset is not in the signed list");
                throw new Fail(Failure.NOT_VERIFIED);
            }
            checkCancelled();

            publish(new State(Phase.DOWNLOADING, null, 0, offer.version, false));
            downloadApk(offer, part, dir);
            if (!part.renameTo(apk)) {
                throw new Fail(Failure.STORAGE);
            }

            publish(new State(Phase.VERIFYING, null, -1, offer.version, false));
            if (ReleaseVerifier.verifyFile(sums, offer.apkName, apk) != ReleaseVerifier.Result.OK) {
                Log.w(TAG, "Downloaded APK does not match the signed checksum");
                throw new Fail(Failure.NOT_VERIFIED);
            }
            checkCancelled();

            markInstalling(context, offer.version);
            publish(new State(Phase.INSTALLING, null, -1, offer.version, false));
            int id;
            try {
                id = backend().install(context, apk);
            } catch (IOException e) {
                throw new Fail(isNoSpace(e) ? Failure.STORAGE : Failure.OTHER);
            }
            committed = true;
            if (cancelled) {
                // Cancelled while the session was being created
                sessionId = -1;
                backend().abandon(context, id);
                clearInstallingMarker(context);
                publish(IDLE_STATE);
            }
            Log.i(TAG, "Install session committed for " + offer.version);
        } catch (Fail f) {
            clearInstallingMarker(context);
            if (cancelled) {
                publish(IDLE_STATE);
            } else {
                Log.w(TAG, "Update failed: " + f.failure);
                publish(new State(Phase.FAILED, f.failure, -1, offer.version, false));
            }
        } catch (Throwable t) {
            clearInstallingMarker(context);
            Log.w(TAG, "Update failed unexpectedly: " + t.getClass().getSimpleName());
            publish(cancelled ? IDLE_STATE : new State(Phase.FAILED, Failure.OTHER, -1, offer.version, false));
        } finally {
            currentCall = null;
            // The session holds its own copy; nothing stays in the cache in any case
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            //noinspection ResultOfMethodCallIgnored
            apk.delete();
            if (!committed && !state.isActive() && state.phase != Phase.FAILED) {
                publish(IDLE_STATE);
            }
        }
    }

    private static void checkCancelled() throws Fail {
        if (cancelled) {
            throw new Fail(Failure.ABORTED);
        }
    }

    private static byte[] fetchBytes(String url, int max) throws Fail {
        try (Response response = open(url)) {
            ResponseBody body = response.body();
            if (body == null || body.contentLength() > max) {
                throw new Fail(Failure.DOWNLOAD);
            }
            okio.BufferedSource source = body.source();
            source.request(max + 1L);
            if (source.getBuffer().size() > max) {
                throw new Fail(Failure.DOWNLOAD);
            }
            return source.readByteArray();
        } catch (Fail f) {
            throw f;
        } catch (IOException e) {
            checkCancelled();
            Log.w(TAG, "Transfer failed: " + e.getClass().getSimpleName());
            throw new Fail(Failure.NO_CONNECTION);
        }
    }

    private static void downloadApk(UpdateChecker.Result offer, File part, File dir) throws Fail {
        try (Response response = open(offer.downloadUrl)) {
            ResponseBody body = response.body();
            if (body == null) {
                throw new Fail(Failure.DOWNLOAD);
            }
            long total = body.contentLength();
            if (total > MAX_APK_BYTES) {
                throw new Fail(Failure.DOWNLOAD);
            }
            if (total > 0 && dir.getUsableSpace() < total + STORAGE_MARGIN_BYTES) {
                throw new Fail(Failure.STORAGE);
            }

            long received = 0;
            int lastPercent = 0;
            byte[] buffer = new byte[32 * 1024];
            InputStream in = body.byteStream();
            try (FileOutputStream out = new FileOutputStream(part)) {
                while (true) {
                    int n;
                    try {
                        n = in.read(buffer);
                    } catch (IOException e) {
                        checkCancelled();
                        Log.w(TAG, "Transfer failed: " + e.getClass().getSimpleName());
                        throw new Fail(Failure.NO_CONNECTION);
                    }
                    if (n < 0) {
                        break;
                    }
                    received += n;
                    if (received > MAX_APK_BYTES) {
                        throw new Fail(Failure.DOWNLOAD);
                    }
                    try {
                        out.write(buffer, 0, n);
                    } catch (IOException e) {
                        throw new Fail(isNoSpace(e) ? Failure.STORAGE : Failure.OTHER);
                    }
                    checkCancelled();
                    int percent = total > 0 ? (int) Math.min(100, received * 100 / total) : -1;
                    if (percent != lastPercent) {
                        lastPercent = percent;
                        publish(new State(Phase.DOWNLOADING, null, percent, offer.version, false));
                    }
                }
                out.getFD().sync();
            } catch (IOException e) {
                throw new Fail(isNoSpace(e) ? Failure.STORAGE : Failure.OTHER);
            }
            if (total > 0 && received != total) {
                throw new Fail(Failure.DOWNLOAD);
            }
            if (received == 0) {
                throw new Fail(Failure.DOWNLOAD);
            }
        } catch (Fail f) {
            throw f;
        } catch (IOException e) {
            checkCancelled();
            Log.w(TAG, "Transfer failed: " + e.getClass().getSimpleName());
            throw new Fail(Failure.NO_CONNECTION);
        }
    }

    /** Opens a URL, following at most MAX_REDIRECTS redirects, each of which must be an allowed address. */
    private static Response open(String url) throws Fail, IOException {
        String current = url;
        for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
            checkCancelled();
            if (!UpdateChecker.isAllowedUrl(current)) {
                Log.w(TAG, "Refusing an address outside the release locations");
                throw new Fail(Failure.DOWNLOAD);
            }
            HttpUrl parsed = HttpUrl.parse(current);
            if (parsed == null) {
                throw new Fail(Failure.DOWNLOAD);
            }
            Request request = new Request.Builder()
                    .url(parsed)
                    .header("User-Agent", "NyxPlay-Android/" + versionName)
                    .header("Accept-Encoding", "identity")
                    .build();
            Call call = client().newCall(request);
            currentCall = call;
            Response response;
            try {
                response = call.execute();
            } catch (IOException first) {
                // A pooled connection the server already closed fails like this; retry once on a fresh one
                checkCancelled();
                client().connectionPool().evictAll();
                call = client().newCall(request);
                currentCall = call;
                response = call.execute();
            }
            if (response.isRedirect()) {
                String location = response.header("Location");
                response.close();
                HttpUrl next = location == null ? null : request.url().resolve(location);
                if (next == null) {
                    throw new Fail(Failure.DOWNLOAD);
                }
                current = next.toString();
                continue;
            }
            if (!response.isSuccessful()) {
                int code = response.code();
                response.close();
                Log.w(TAG, "Download answered HTTP " + code);
                throw new Fail(Failure.DOWNLOAD);
            }
            return response;
        }
        throw new Fail(Failure.DOWNLOAD);
    }

    private static boolean isNoSpace(IOException e) {
        String message = e.getMessage();
        return message != null && (message.contains("ENOSPC") || message.toLowerCase(java.util.Locale.ROOT).contains("no space"));
    }

    // ---- Install result (from UpdateInstallReceiver) ----

    /** Called by the backend before it commits, so a status that arrives at once is matched. */
    static void sessionCreated(int id) {
        sessionId = id;
    }

    /** Handles a PackageInstaller status broadcast; ignores anything that does not belong to the current session. */
    static synchronized void onInstallStatus(Context context, int status, int statusSessionId, Intent confirmation) {
        State current = state;
        if (current.phase != Phase.INSTALLING || statusSessionId != sessionId) {
            return;
        }
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            if (confirmation != null) {
                confirmIntent = confirmation;
                publish(new State(Phase.INSTALLING, null, -1, current.version, true));
            }
            return;
        }
        sessionId = -1;
        confirmIntent = null;
        if (status == PackageInstaller.STATUS_SUCCESS) {
            // The app is normally replaced right now; "Updated to" is shown after the restart
            Log.i(TAG, "Install succeeded");
            publish(IDLE_STATE);
            return;
        }
        clearInstallingMarker(context);
        Failure failure = failureForStatus(status);
        Log.w(TAG, "Install failed with status " + status);
        publish(new State(Phase.FAILED, failure, -1, current.version, false));
    }

    static Failure failureForStatus(int status) {
        switch (status) {
            case PackageInstaller.STATUS_FAILURE_BLOCKED:
                return Failure.BLOCKED;
            case PackageInstaller.STATUS_FAILURE_CONFLICT:
                return Failure.CONFLICT;
            case PackageInstaller.STATUS_FAILURE_STORAGE:
                return Failure.STORAGE;
            case PackageInstaller.STATUS_FAILURE_INCOMPATIBLE:
                return Failure.INCOMPATIBLE;
            case PackageInstaller.STATUS_FAILURE_ABORTED:
                return Failure.ABORTED;
            default:
                return Failure.OTHER;
        }
    }

    // ---- Files and markers ----

    static File updateDir(Context context) {
        return new File(context.getCacheDir(), "update");
    }

    /** Removes download leftovers; does nothing while a run is active. */
    public static void cleanupLeftovers(Context context) {
        if (state.isActive()) {
            return;
        }
        clearFiles(updateDir(context.getApplicationContext()));
    }

    private static void clearFiles(File dir) {
        File[] files = dir.listFiles();
        if (files == null) {
            return;
        }
        for (File f : files) {
            //noinspection ResultOfMethodCallIgnored
            f.delete();
        }
    }

    private static void abandonOwnSessions(Context context) {
        try {
            PackageInstaller installer = context.getPackageManager().getPackageInstaller();
            for (PackageInstaller.SessionInfo info : installer.getMySessions()) {
                try {
                    installer.abandonSession(info.getSessionId());
                } catch (RuntimeException ignored) {
                    // Already gone or not ours to touch
                }
            }
        } catch (RuntimeException ignored) {
            // No installer service (should not happen on a phone)
        }
    }

    private static SharedPreferences marker(Context context) {
        return context.getApplicationContext().getSharedPreferences("update_check", Context.MODE_PRIVATE);
    }

    private static void markInstalling(Context context, String version) {
        marker(context).edit().putString(KEY_INSTALLING_VERSION, version)
                .putLong(KEY_INSTALLING_AT, System.currentTimeMillis()).apply();
    }

    private static void clearInstallingMarker(Context context) {
        marker(context).edit().remove(KEY_INSTALLING_VERSION).remove(KEY_INSTALLING_AT).apply();
    }

    /**
     * Returns the version this app was just updated to, once. The marker is written when an install
     * is committed and compared with the running version after the restart.
     */
    public static String consumeUpdatedVersion(Context context) {
        SharedPreferences prefs = marker(context);
        String version = prefs.getString(KEY_INSTALLING_VERSION, "");
        if (version.isEmpty()) {
            return null;
        }
        long at = prefs.getLong(KEY_INSTALLING_AT, 0);
        long now = System.currentTimeMillis();
        boolean fresh = at > 0 && at <= now && now - at < MARKER_MAX_AGE_MS;
        if (state.phase == Phase.INSTALLING) {
            // The install is still being confirmed
            return null;
        }
        int[] target = UpdateChecker.parseInstalledVersion(version);
        int[] running = UpdateChecker.parseInstalledVersion(versionName);
        boolean updated = target != null && running != null && UpdateChecker.compareVersions(running, target) >= 0;
        if (updated || !fresh) {
            clearInstallingMarker(context);
        }
        return updated ? version : null;
    }

    static boolean isNewerThanInstalled(String version) {
        int[] offered = UpdateChecker.parseInstalledVersion(version);
        int[] running = UpdateChecker.parseInstalledVersion(versionName);
        return offered != null && running != null && UpdateChecker.compareVersions(offered, running) > 0;
    }

    // ---- Wiring ----

    private static synchronized Executor executor() {
        if (executor != null) {
            return executor;
        }
        if (defaultExecutor == null) {
            defaultExecutor = Executors.newSingleThreadExecutor(new ThreadFactory() {
                @Override
                public Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "NyxPlay-UpdateInstall");
                    t.setDaemon(true);
                    return t;
                }
            });
        }
        return defaultExecutor;
    }

    private static synchronized OkHttpClient client() {
        if (client == null) {
            // Platform trust; redirects are followed by hand so each hop is checked against the allow-list
            client = new OkHttpClient.Builder()
                    .connectTimeout(15, TimeUnit.SECONDS)
                    .readTimeout(30, TimeUnit.SECONDS)
                    .writeTimeout(30, TimeUnit.SECONDS)
                    .followRedirects(false)
                    .followSslRedirects(false)
                    .build();
        }
        return client;
    }

    private static synchronized Backend backend() {
        if (backend == null) {
            backend = new ApkInstaller();
        }
        return backend;
    }

    /** Test hook: puts the flow into the install phase of a committed session. */
    static synchronized void setInstallingForTest(String version, int id) {
        sessionId = id;
        publish(new State(Phase.INSTALLING, null, -1, version, false));
    }

    /** Test hook: replaces collaborators (null restores defaults) and resets the state. */
    static synchronized void setTestHooks(Executor testExecutor, OkHttpClient testClient, Backend testBackend, String testVersionName) {
        executor = testExecutor;
        client = testClient;
        backend = testBackend;
        versionName = testVersionName == null ? BuildConfig.VERSION_NAME : testVersionName;
        cancelled = false;
        currentCall = null;
        sessionId = -1;
        confirmIntent = null;
        state = IDLE_STATE;
        listeners.clear();
    }
}
