package com.limelight.nvstream.mdns;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.concurrent.atomic.AtomicInteger;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class MdnsDiscoveryAgentTest {
    private static class TestAgent extends MdnsDiscoveryAgent {
        TestAgent(MdnsDiscoveryListener listener) {
            super(listener);
        }

        @Override
        public void startDiscovery(int discoveryIntervalMs) {}

        @Override
        public void stopDiscovery() {}

        void announce() throws Exception {
            announce("host", "10.0.0.5");
        }

        void announce(String name, String address) throws Exception {
            reportNewComputer(name, 47989,
                    new Inet4Address[] { (Inet4Address) InetAddress.getByName(address) }, new Inet6Address[0]);
        }
    }

    @Test
    public void mdnsAdd_failureThenSuccess_secondAnnounceRetries() throws Exception {
        final AtomicInteger calls = new AtomicInteger();
        TestAgent agent = new TestAgent(new MdnsDiscoveryListener() {
            @Override
            public boolean notifyComputerAdded(MdnsComputer computer) {
                // The first attempt fails, later ones succeed
                return calls.incrementAndGet() > 1;
            }

            @Override
            public void notifyDiscoveryFailure(Exception e) {}
        });
        agent.probeBackoffMs = 0;

        agent.announce();
        assertEquals(1, calls.get());
        assertEquals(0, agent.getComputerSet().size());

        agent.announce();
        assertEquals(2, calls.get());
        assertEquals(1, agent.getComputerSet().size());

        // A known host is not reported again
        agent.announce();
        assertEquals(2, calls.get());
    }

    @Test
    public void failedProbeIsNotRepeatedDuringTheBackoff() throws Exception {
        final AtomicInteger calls = new AtomicInteger();
        TestAgent agent = new TestAgent(new MdnsDiscoveryListener() {
            @Override
            public boolean notifyComputerAdded(MdnsComputer computer) {
                calls.incrementAndGet();
                return false;
            }

            @Override
            public void notifyDiscoveryFailure(Exception e) {}
        });

        for (int i = 0; i < 5; i++) {
            agent.announce();
        }
        assertEquals(1, calls.get());
    }

    @Test
    public void automaticallyAddedHostsAreCapped() throws Exception {
        final AtomicInteger calls = new AtomicInteger();
        TestAgent agent = new TestAgent(new MdnsDiscoveryListener() {
            @Override
            public boolean notifyComputerAdded(MdnsComputer computer) {
                calls.incrementAndGet();
                return true;
            }

            @Override
            public void notifyDiscoveryFailure(Exception e) {}
        });

        int total = MdnsDiscoveryAgent.MAX_AUTO_ADDED_COMPUTERS + 10;
        for (int i = 0; i < total; i++) {
            agent.announce("host" + i, "10.0.1." + (i + 1));
        }
        assertEquals(MdnsDiscoveryAgent.MAX_AUTO_ADDED_COMPUTERS, calls.get());
        assertEquals(MdnsDiscoveryAgent.MAX_AUTO_ADDED_COMPUTERS, agent.getComputerSet().size());
    }

    @Test
    public void listenerThrows_entryIsForgotten() throws Exception {
        final AtomicInteger calls = new AtomicInteger();
        TestAgent agent = new TestAgent(new MdnsDiscoveryListener() {
            @Override
            public boolean notifyComputerAdded(MdnsComputer computer) {
                calls.incrementAndGet();
                throw new IllegalStateException("boom");
            }

            @Override
            public void notifyDiscoveryFailure(Exception e) {}
        });

        try {
            agent.announce();
        } catch (IllegalStateException expected) {
            // expected
        }
        assertEquals(0, agent.getComputerSet().size());
    }

    @Test
    public void listenerRunsOutsideTheComputersLock() throws Exception {
        final TestAgent[] holder = new TestAgent[1];
        final int[] size = { -1 };
        holder[0] = new TestAgent(new MdnsDiscoveryListener() {
            @Override
            public boolean notifyComputerAdded(MdnsComputer computer) {
                // Another thread must be able to read the set while the listener blocks
                Thread reader = new Thread(() -> size[0] = holder[0].getComputerSet().size());
                reader.start();
                try {
                    reader.join(2000);
                } catch (InterruptedException ignored) {}
                return true;
            }

            @Override
            public void notifyDiscoveryFailure(Exception e) {}
        });

        holder[0].announce();
        assertEquals(1, size[0]);
    }
}
