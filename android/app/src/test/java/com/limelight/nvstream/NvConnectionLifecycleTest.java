package com.limelight.nvstream;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.anyString;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.av.video.VideoDecoderRenderer;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.jni.MoonBridge;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Covers the start/stop bookkeeping of NvConnection: one native connection at a time, a permit
 * that is released exactly once, and a start thread that gives up once stop() was called.
 */
@Config(sdk = {33}, shadows = {NvConnectionLifecycleTest.LifecycleBridge.class})
@RunWith(RobolectricTestRunner.class)
public class NvConnectionLifecycleTest {

    @Implements(value = MoonBridge.class, isInAndroidSdk = false)
    public static class LifecycleBridge {
        static final AtomicInteger startCalls = new AtomicInteger();
        static final AtomicInteger stopCalls = new AtomicInteger();
        static volatile int startResult = 0;

        static void reset() {
            startCalls.set(0);
            stopCalls.set(0);
            startResult = 0;
        }

        @Implementation
        protected static void __staticInitializer__() {
            // no native library on the JVM
        }

        @Implementation
        protected static int startConnection(String address, String appVersion, String gfeVersion,
                                             String rtspSessionUrl, int serverCodecModeSupport,
                                             int width, int height, int fps,
                                             int bitrate, int packetSize, int streamingRemotely,
                                             int audioConfiguration, int supportedVideoFormats,
                                             int clientRefreshRateX100,
                                             byte[] riAesKey, byte[] riAesIv,
                                             int videoCapabilities,
                                             int colorSpace, int colorRange) {
            startCalls.incrementAndGet();
            return startResult;
        }

        @Implementation
        protected static void stopConnection() {
            stopCalls.incrementAndGet();
        }

        @Implementation
        protected static void interruptConnection() {
        }
    }

    /** NvConnection whose HTTP launch step is replaced by a test hook. */
    private static class TestConnection extends NvConnection {
        interface Launch {
            boolean run() throws Exception;
        }

        final AtomicInteger launchCalls = new AtomicInteger();
        volatile Launch launch = () -> true;

        TestConnection() {
            super(RuntimeEnvironment.getApplication(),
                    new ComputerDetails.AddressTuple("127.0.0.1", 47989), 47984, "test-id",
                    new StreamConfiguration.Builder()
                            .setAudioConfiguration(new MoonBridge.AudioConfiguration(2, 0x3))
                            .build(),
                    null, null);
        }

        @Override
        boolean startApp() throws XmlPullParserException, IOException {
            launchCalls.incrementAndGet();
            try {
                return launch.run();
            } catch (XmlPullParserException | IOException | RuntimeException e) {
                throw e;
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        }
    }

    private NvConnectionListener listener;
    private VideoDecoderRenderer video;
    private AudioRenderer audio;

    @Before
    public void setUp() throws Exception {
        LifecycleBridge.reset();
        setPermits(1);
        listener = mock(NvConnectionListener.class);
        when(listener.stageFailed(anyString(), anyInt(), anyInt())).thenReturn(false);
        video = mock(VideoDecoderRenderer.class);
        audio = mock(AudioRenderer.class);
    }

    @After
    public void tearDown() throws Exception {
        setPermits(1);
    }

    private static Semaphore semaphore() throws Exception {
        Field f = NvConnection.class.getDeclaredField("connectionAllowed");
        f.setAccessible(true);
        return (Semaphore) f.get(null);
    }

    private static void setPermits(int permits) throws Exception {
        Field f = NvConnection.class.getDeclaredField("connectionAllowed");
        f.setAccessible(true);
        f.set(null, new Semaphore(permits));
    }

    private static void waitFor(java.util.function.BooleanSupplier condition, long timeoutMs) throws Exception {
        long end = System.currentTimeMillis() + timeoutMs;
        while (!condition.getAsBoolean() && System.currentTimeMillis() < end) {
            Thread.sleep(10);
        }
    }

    @Test
    public void stopBeforeAcquireDoesNotStartConnection() throws Exception {
        TestConnection conn = new TestConnection();
        CountDownLatch inLaunch = new CountDownLatch(1);
        CountDownLatch proceed = new CountDownLatch(1);
        conn.launch = () -> {
            inLaunch.countDown();
            proceed.await();
            return true;
        };

        conn.start(audio, video, listener);
        assertTrue(inLaunch.await(2, TimeUnit.SECONDS));

        conn.stop();
        proceed.countDown();

        Thread.sleep(300);
        assertEquals(0, LifecycleBridge.startCalls.get());
        assertEquals(1, semaphore().availablePermits());
    }

    @Test
    public void stopDuringRetryWaitCancelsFurtherLaunchRequests() throws Exception {
        TestConnection conn = new TestConnection();
        when(listener.stageFailed(anyString(), anyInt(), anyInt())).thenReturn(true);
        conn.launch = () -> {
            throw new IOException("host unreachable");
        };

        conn.start(audio, video, listener);
        waitFor(() -> conn.launchCalls.get() >= 1, 2000);
        conn.stop();

        // The retry wait is 2 s; after stop() no further launch request may be sent
        Thread.sleep(2600);
        assertEquals(1, conn.launchCalls.get());
        assertEquals(0, LifecycleBridge.startCalls.get());
        assertEquals(1, semaphore().availablePermits());
    }

    @Test
    public void failedStartReleasesPermitOnlyOnce() throws Exception {
        TestConnection conn = new TestConnection();
        LifecycleBridge.startResult = -1;

        conn.start(audio, video, listener);
        waitFor(() -> LifecycleBridge.startCalls.get() == 1, 3000);
        waitFor(() -> {
            try {
                return semaphore().availablePermits() == 1;
            } catch (Exception e) {
                return false;
            }
        }, 1000);
        assertEquals(1, semaphore().availablePermits());

        // The activity still calls stop() afterwards; that must not add a second permit
        conn.stop();
        assertEquals(1, semaphore().availablePermits());
    }

    @Test
    public void stopTwiceReleasesOnce() throws Exception {
        TestConnection conn = new TestConnection();

        conn.start(audio, video, listener);
        waitFor(() -> LifecycleBridge.startCalls.get() == 1, 3000);
        assertEquals(0, semaphore().availablePermits());

        conn.stop();
        conn.stop();
        assertEquals(1, semaphore().availablePermits());
    }

    @Test
    public void stopWithoutStartDoesNotCreatePermit() throws Exception {
        TestConnection conn = new TestConnection();

        conn.stop();
        assertEquals(1, semaphore().availablePermits());
    }

    @Test
    public void secondConnectionWaitsForFirst() throws Exception {
        TestConnection first = new TestConnection();
        TestConnection second = new TestConnection();

        first.start(audio, video, listener);
        waitFor(() -> LifecycleBridge.startCalls.get() == 1, 3000);

        second.start(audio, video, listener);
        Thread.sleep(400);
        assertEquals(1, LifecycleBridge.startCalls.get());

        first.stop();
        waitFor(() -> LifecycleBridge.startCalls.get() == 2, 3000);
        assertEquals(2, LifecycleBridge.startCalls.get());

        second.stop();
        assertEquals(1, semaphore().availablePermits());
    }

    @Test
    public void interruptedStartThreadDoesNotThrow() throws Exception {
        TestConnection conn = new TestConnection();
        final AtomicReference<Thread> startThread = new AtomicReference<>();
        when(listener.stageFailed(anyString(), anyInt(), anyInt())).thenAnswer(inv -> {
            startThread.set(Thread.currentThread());
            return true;
        });
        conn.launch = () -> false;

        final AtomicInteger uncaught = new AtomicInteger();
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> uncaught.incrementAndGet());
        try {
            conn.start(audio, video, listener);
            waitFor(() -> startThread.get() != null, 2000);
            assertNotNull(startThread.get());

            startThread.get().interrupt();
            Thread.sleep(500);
        } finally {
            Thread.setDefaultUncaughtExceptionHandler(previous);
            conn.stop();
        }
        assertEquals(0, uncaught.get());
    }
}
