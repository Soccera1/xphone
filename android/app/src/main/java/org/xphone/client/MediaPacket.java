package org.xphone.client;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

final class MediaPacket {
    final long call;
    final int generation, sequence, channels;
    final byte[] opus;

    MediaPacket(long call, int generation, int sequence, int channels, byte[] opus) {
        this.call = call;
        this.generation = generation;
        this.sequence = sequence;
        this.channels = channels;
        this.opus = opus;
    }

    byte[] pack() {
        if (call <= 0 || generation <= 0 || channels < 1 || channels > 2 ||
                opus.length < 1 || opus.length > 1076) throw new IllegalArgumentException("Invalid audio record");
        return ByteBuffer.allocate(24 + opus.length).order(ByteOrder.BIG_ENDIAN)
                .put((byte) 1).put((byte) channels).putShort((short) 0).putLong(call)
                .putInt(generation).putInt(sequence).putInt(sequence * 960).put(opus).array();
    }

    static MediaPacket parse(byte[] data) {
        if (data.length < 25 || data.length > 1100) return null;
        ByteBuffer b = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN);
        if (b.get() != 1) return null;
        int channels = b.get() & 255;
        if (channels < 1 || channels > 2 || b.getShort() != 0) return null;
        long call = b.getLong();
        int generation = b.getInt(), sequence = b.getInt(), timestamp = b.getInt();
        if (call <= 0 || generation <= 0 || timestamp != sequence * 960) return null;
        return new MediaPacket(call, generation, sequence, channels, Arrays.copyOfRange(data, 24, data.length));
    }
}
