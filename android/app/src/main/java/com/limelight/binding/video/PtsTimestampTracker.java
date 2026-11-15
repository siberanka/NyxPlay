package com.limelight.binding.video;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Remembers when each frame was queued to the decoder, keyed by presentation timestamp.
 * Safe to use from the input and renderer threads; the number of entries is bounded so
 * frames that never produce output cannot accumulate.
 */
final class PtsTimestampTracker {
    static final int DEFAULT_CAPACITY = 256;

    private final int capacity;
    private final LinkedHashMap<Long, Long> entries = new LinkedHashMap<>();

    PtsTimestampTracker() {
        this(DEFAULT_CAPACITY);
    }

    PtsTimestampTracker(int capacity) {
        this.capacity = Math.max(1, capacity);
    }

    synchronized void put(long ptsUs, long enqueueNs) {
        // Re-insert so a reused timestamp counts as the most recent entry
        entries.remove(ptsUs);
        entries.put(ptsUs, enqueueNs);
        while (entries.size() > capacity) {
            java.util.Iterator<Map.Entry<Long, Long>> it = entries.entrySet().iterator();
            it.next();
            it.remove();
        }
    }

    /** Returns the stored enqueue time and forgets it, or -1 if there is none. */
    synchronized long remove(long ptsUs) {
        Long value = entries.remove(ptsUs);
        return value != null ? value : -1L;
    }

    synchronized void clear() {
        entries.clear();
    }

    synchronized int size() {
        return entries.size();
    }
}
