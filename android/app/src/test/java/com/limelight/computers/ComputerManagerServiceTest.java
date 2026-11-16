package com.limelight.computers;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.ContentValues;
import android.content.Context;
import android.database.sqlite.SQLiteDatabase;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.TestHostResponses;
import com.limelight.binding.crypto.AndroidCryptoProvider;
import com.limelight.nvstream.http.ComputerDetails;

import okhttp3.mockwebserver.Dispatcher;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.mockwebserver.RecordedRequest;
import okhttp3.mockwebserver.SocketPolicy;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ServiceController;
import org.robolectric.annotation.Config;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.LinkedList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class ComputerManagerServiceTest {
    private static final String HOST_ID = "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f";

    private Context context;
    private MockWebServer server;
    private ServiceController<ComputerManagerService> controller;
    private ComputerManagerService.ComputerManagerBinder binder;

    @Before
    public void setUp() throws Exception {
        context = ApplicationProvider.getApplicationContext();
        context.deleteDatabase("computers4.db");
        server = new MockWebServer();
        server.start();
    }

    @After
    public void tearDown() throws Exception {
        if (controller != null) {
            controller.destroy();
        }
        server.shutdown();
    }

    private void startService() {
        controller = Robolectric.buildService(ComputerManagerService.class).create();
        binder = (ComputerManagerService.ComputerManagerBinder) controller.get().onBind(null);
    }

    private ComputerDetails newCandidate() {
        ComputerDetails details = new ComputerDetails();
        details.name = "Candidate";
        details.localAddress = new ComputerDetails.AddressTuple(server.getHostName(), server.getPort());
        return details;
    }

    private static MockResponse serverInfo(String uuid) {
        return new MockResponse().setBody(TestHostResponses.serverInfo(uuid));
    }

    private static MockResponse reset() {
        return new MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START);
    }

    private int storedHostCount() {
        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        try {
            return manager.getAllComputers().size();
        } finally {
            manager.close();
        }
    }

    @Test
    public void addComputerBlocking_allPollsOk_succeeds() throws Exception {
        startService();
        server.enqueue(serverInfo(HOST_ID));
        server.enqueue(serverInfo(HOST_ID));

        assertTrue(binder.addComputerBlocking(newCandidate()));
        assertEquals(2, server.getRequestCount());
        assertEquals(1, storedHostCount());
        assertNotNull(binder.getComputer(HOST_ID));
    }

    @Test
    public void addComputerBlocking_firstPollOk_secondResetOnce_stillSucceeds() throws Exception {
        startService();
        server.enqueue(serverInfo(HOST_ID));
        server.enqueue(reset());
        server.enqueue(serverInfo(HOST_ID));

        assertTrue(binder.addComputerBlocking(newCandidate()));
        assertEquals(1, storedHostCount());
    }

    @Test
    public void addComputerBlocking_firstPollReset_retriesWholeSequence() throws Exception {
        startService();
        server.enqueue(reset());
        server.enqueue(serverInfo(HOST_ID));
        server.enqueue(serverInfo(HOST_ID));

        assertTrue(binder.addComputerBlocking(newCandidate()));
        assertEquals(1, storedHostCount());
    }

    @Test
    public void addComputerBlocking_hostNeverAnswers_failsAfterBoundedAttempts() throws Exception {
        startService();
        server.setDispatcher(new Dispatcher() {
            @Override
            public MockResponse dispatch(RecordedRequest request) {
                return new MockResponse().setBody(TestHostResponses.serverInfo(HOST_ID))
                        .setSocketPolicy(SocketPolicy.DISCONNECT_DURING_RESPONSE_BODY);
            }
        });

        long start = System.currentTimeMillis();
        assertFalse(binder.addComputerBlocking(newCandidate()));
        // Two extra attempts, each preceded by a pause
        assertTrue(System.currentTimeMillis() - start >= 550);
        assertEquals(0, storedHostCount());
    }

    @Test
    public void addComputerBlocking_invalidUuid_returnsFalse_noRetry_noDbRow() throws Exception {
        startService();
        server.setDispatcher(new Dispatcher() {
            @Override
            public MockResponse dispatch(RecordedRequest request) {
                return serverInfo("../../x");
            }
        });

        assertFalse(binder.addComputerBlocking(newCandidate()));
        assertEquals(1, server.getRequestCount());
        assertEquals(0, storedHostCount());
    }

    @Test
    public void setServerCert_persistsCertificate() throws Exception {
        startService();
        server.enqueue(serverInfo(HOST_ID));
        server.enqueue(serverInfo(HOST_ID));
        assertTrue(binder.addComputerBlocking(newCandidate()));

        java.security.cert.X509Certificate cert = new AndroidCryptoProvider(context).getClientCertificate();
        assertTrue(binder.setServerCert(HOST_ID, cert));
        assertEquals(cert, binder.getComputer(HOST_ID).serverCert);

        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        try {
            assertEquals(cert, manager.getComputerByUUID(HOST_ID).serverCert);
        } finally {
            manager.close();
        }
    }

    @Test
    public void setServerCert_unknownComputer_returnsFalse() throws Exception {
        startService();
        java.security.cert.X509Certificate cert = new AndroidCryptoProvider(context).getClientCertificate();
        assertFalse(binder.setServerCert(HOST_ID, cert));
    }

    @Test
    public void takeDroppedHostCount_reportsInvalidRowsOnce() throws Exception {
        SQLiteDatabase db = context.openOrCreateDatabase("computers4.db", 0, null);
        try {
            db.execSQL("CREATE TABLE IF NOT EXISTS Computers(UUID TEXT PRIMARY KEY, ComputerName TEXT NOT NULL, " +
                    "Addresses TEXT NOT NULL, MacAddress TEXT, ServerCert TEXT)");
            ContentValues values = new ContentValues();
            values.put("UUID", "host_1");
            values.put("ComputerName", "stub");
            values.put("Addresses", "{}");
            db.insert("Computers", null, values);
        } finally {
            db.close();
        }

        startService();
        assertEquals(1, binder.takeDroppedHostCount());
        assertEquals(0, binder.takeDroppedHostCount());
    }

    @Test
    public void invalidateStateForComputer_doesNotBlockGetComputer_whileSlowPoll() throws Exception {
        startService();
        server.enqueue(serverInfo(HOST_ID));
        server.enqueue(serverInfo(HOST_ID));
        assertTrue(binder.addComputerBlocking(newCandidate()));

        Field tuplesField = ComputerManagerService.class.getDeclaredField("pollingTuples");
        tuplesField.setAccessible(true);
        @SuppressWarnings("unchecked")
        final LinkedList<PollingTuple> tuples = (LinkedList<PollingTuple>) tuplesField.get(controller.get());
        final PollingTuple tuple = tuples.getFirst();

        // Simulate a poll that holds the network lock for a long time
        final CountDownLatch locked = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);
        Thread holder = new Thread(() -> {
            synchronized (tuple.networkLock) {
                locked.countDown();
                try {
                    release.await(10, TimeUnit.SECONDS);
                } catch (InterruptedException ignored) {}
            }
        });
        holder.start();
        assertTrue(locked.await(2, TimeUnit.SECONDS));

        Thread invalidator = new Thread(() -> binder.invalidateStateForComputer(HOST_ID));
        invalidator.start();
        Thread.sleep(200);

        final ComputerDetails[] result = new ComputerDetails[1];
        Thread reader = new Thread(() -> result[0] = binder.getComputer(HOST_ID));
        long start = System.currentTimeMillis();
        reader.start();
        reader.join(2000);
        long elapsed = System.currentTimeMillis() - start;

        release.countDown();
        holder.join(2000);
        invalidator.join(2000);

        assertFalse("getComputer was blocked", reader.isAlive());
        assertTrue("getComputer took " + elapsed + " ms", elapsed < 1000);
        assertNotNull(result[0]);
        assertEquals(ComputerDetails.State.UNKNOWN, result[0].state);
    }

    @Test
    public void applistPoller_keepsFailing_reportsFailureOnceAndAgainAfterReset() throws Exception {
        startService();

        // No usable address, so every poll fails the same way a rejected response does
        ComputerDetails details = new ComputerDetails();
        details.name = "Host";
        details.uuid = HOST_ID;
        details.state = ComputerDetails.State.ONLINE;
        details.pairState = com.limelight.nvstream.http.PairingManager.PairState.PAIRED;

        final CountDownLatch firstReport = new CountDownLatch(1);
        final CountDownLatch secondReport = new CountDownLatch(2);
        ComputerManagerService.ApplistPoller poller = binder.createAppListPoller(details);
        poller.setFailureListener(() -> {
            firstReport.countDown();
            secondReport.countDown();
        });
        poller.start();

        try {
            assertTrue("no failure report", firstReport.await(15, TimeUnit.SECONDS));

            poller.resetFailures();
            assertTrue("no second report after a reset", secondReport.await(15, TimeUnit.SECONDS));
        } finally {
            poller.stop();
        }
    }

    @Test
    public void sharedPollingFields_areVolatile() throws Exception {
        for (String name : new String[] { "listener", "pollingActive" }) {
            Field f = ComputerManagerService.class.getDeclaredField(name);
            assertTrue(name + " must be volatile", Modifier.isVolatile(f.getModifiers()));
        }
    }

    @Test
    public void getComputer_unknown_returnsNull() throws Exception {
        startService();
        assertNull(binder.getComputer("does-not-exist"));
    }
}
