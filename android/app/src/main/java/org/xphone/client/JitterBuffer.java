package org.xphone.client;

import java.util.HashMap;

/** At most twelve packets, 60 ms startup, modulo-2^32 ordering and bounded resync. */
final class JitterBuffer {
    private final HashMap<Integer, byte[]> frames = new HashMap<>();
    private int next;
    private boolean started;
    private long firstAt;

    synchronized void offer(int sequence, byte[] opus, long now) {
        if (!started && frames.isEmpty()) { next = sequence; firstAt = now; }
        int distance = sequence - next;
        if (distance < 0) return;
        if (distance > 50) {
            frames.clear(); next = sequence; firstAt = now; started = false;
        }
        if (frames.size() < 12) frames.putIfAbsent(sequence, opus);
    }

    synchronized boolean ready(long now) {
        if (!started && !frames.isEmpty() && now - firstAt >= 60) started = true;
        return started;
    }

    synchronized byte[] take() { return frames.remove(next++); }
}
