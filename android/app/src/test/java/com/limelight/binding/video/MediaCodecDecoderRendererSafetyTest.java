package com.limelight.binding.video;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicReference;

public class MediaCodecDecoderRendererSafetyTest {

    @Test
    public void trackerReturnsAndForgetsStoredValue() {
        PtsTimestampTracker tracker = new PtsTimestampTracker();
        tracker.put(1000L, 42L);

        assertEquals(42L, tracker.remove(1000L));
        assertEquals(-1L, tracker.remove(1000L));
        assertEquals(-1L, tracker.remove(2000L));
    }

    @Test
    public void trackerEvictsOldestEntriesWhenFull() {
        PtsTimestampTracker tracker = new PtsTimestampTracker();
        for (int i = 0; i < 300; i++) {
            tracker.put(i, i + 1000L);
        }

        assertTrue(tracker.size() <= PtsTimestampTracker.DEFAULT_CAPACITY);
        assertEquals(-1L, tracker.remove(0));
        assertEquals(299L + 1000L, tracker.remove(299));
    }

    @Test
    public void trackerClearForgetsEverything() {
        PtsTimestampTracker tracker = new PtsTimestampTracker();
        tracker.put(1, 1);
        tracker.put(2, 2);
        tracker.clear();

        assertEquals(0, tracker.size());
        assertEquals(-1L, tracker.remove(1));
    }

    @Test
    public void trackerSurvivesConcurrentUse() throws Exception {
        final PtsTimestampTracker tracker = new PtsTimestampTracker();
        final AtomicReference<Throwable> failure = new AtomicReference<>();

        Thread producer = new Thread(() -> {
            try {
                for (int i = 0; i < 100_000; i++) {
                    tracker.put(i, i);
                }
            } catch (Throwable t) {
                failure.set(t);
            }
        });
        Thread consumer = new Thread(() -> {
            try {
                for (int i = 0; i < 100_000; i++) {
                    tracker.remove(i);
                }
            } catch (Throwable t) {
                failure.set(t);
            }
        });
        producer.start();
        consumer.start();
        producer.join();
        consumer.join();

        assertNull(failure.get());
        assertTrue(tracker.size() <= PtsTimestampTracker.DEFAULT_CAPACITY);
    }

    @Test
    public void hdrInfoRejectsMissingOrShortMetadata() {
        assertNull(HdrStaticInfo.build(null));
        assertNull(HdrStaticInfo.build(new byte[0]));
        assertNull(HdrStaticInfo.build(new byte[23]));
    }

    @Test
    public void hdrInfoBuildsLittleEndianBlockWithLeadingType() {
        byte[] metadata = new byte[24];
        for (int i = 0; i < metadata.length; i++) {
            metadata[i] = (byte) (i + 1);
        }

        ByteBuffer info = HdrStaticInfo.build(metadata);

        assertNotNull(info);
        assertEquals(25, info.remaining());
        info.order(ByteOrder.LITTLE_ENDIAN);
        assertEquals(0, info.get(0));
        // The remaining 24 bytes are the host metadata unchanged
        for (int i = 0; i < 24; i++) {
            assertEquals(metadata[i], info.get(i + 1));
        }
    }

    @Test
    public void hdrInfoIgnoresTrailingBytes() {
        ByteBuffer info = HdrStaticInfo.build(new byte[32]);

        assertNotNull(info);
        assertEquals(25, info.remaining());
    }
}
