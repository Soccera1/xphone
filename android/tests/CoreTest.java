package org.xphone.client;

import java.util.Arrays;

/** Runs on the host JVM without Android mocks. */
public final class CoreTest {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) {
        MediaPacket original = new MediaPacket(Long.MAX_VALUE, 7, -1, 2, new byte[] {3, 4, 5});
        MediaPacket copy = MediaPacket.parse(original.pack());
        check(copy != null && copy.call == original.call && copy.generation == 7 && copy.sequence == -1);
        check(copy.channels == 2 && Arrays.equals(copy.opus, original.opus));
        byte[] malformed = original.pack(); malformed[2] = 1;
        check(MediaPacket.parse(malformed) == null);
        malformed = original.pack(); malformed[23] ^= 1;
        check(MediaPacket.parse(malformed) == null);
        check(MediaPacket.parse(new byte[24]) == null);
        check(MediaPacket.parse(new byte[1101]) == null);

        JitterBuffer jitter = new JitterBuffer();
        jitter.offer(-1, new byte[] {1}, 100);
        jitter.offer(1, new byte[] {3}, 110);
        jitter.offer(0, new byte[] {2}, 120);
        jitter.offer(0, new byte[] {9}, 120);
        check(!jitter.ready(159) && jitter.ready(160));
        check(jitter.take()[0] == 1 && jitter.take()[0] == 2 && jitter.take()[0] == 3);
        check(jitter.take() == null); // packet-loss concealment request
        jitter.offer(1, new byte[] {4}, 180); // late packet
        check(jitter.take() == null);
        jitter.offer(1000, new byte[] {5}, 200); // bounded resync
        check(!jitter.ready(259) && jitter.ready(260) && jitter.take()[0] == 5);

        for (int channels : new int[] {1, 2}) {
            long codec = NativeBridge.codecCreate(channels, channels);
            check(codec != 0);
            double left = 0, right = 0, difference = 0;
            for (int frame = 0; frame < 30; ++frame) {
                short[] input = new short[960 * channels];
                for (int i = 0; i < 960; ++i) {
                    input[i * channels] = (short) (5000 * Math.sin(2 * Math.PI * 440 * (frame * 960 + i) / 48000));
                    if (channels == 2) input[i * channels + 1] = (short) (4000 * Math.sin(2 * Math.PI * 880 * (frame * 960 + i) / 48000));
                }
                byte[] encoded = NativeBridge.encode(codec, input);
                check(encoded != null && encoded.length <= 1076);
                short[] decoded = NativeBridge.decode(codec, encoded);
                check(decoded != null && decoded.length == input.length);
                if (frame > 5) for (int i = 0; i < 960; ++i) {
                    left += Math.abs(decoded[i * channels]);
                    if (channels == 2) {
                        right += Math.abs(decoded[i * channels + 1]);
                        difference += Math.abs(decoded[i * channels] - decoded[i * channels + 1]);
                    }
                }
            }
            check(left > 100000 && (channels == 1 || (right > 100000 && difference > 100000)));
            check(NativeBridge.decode(codec, null).length == 960 * channels);
            check(NativeBridge.decode(codec, new byte[1077]) == null);
            check(NativeBridge.encode(codec, new short[1]) == null);
            NativeBridge.codecDestroy(codec);
        }
        check(NativeBridge.codecCreate(0, 3) == 0);
        NativeBridge.Listener listener = new NativeBridge.Listener() {
            public void onControl(String json) { throw new AssertionError(json); }
            public void onMedia(byte[] record) { throw new AssertionError(); }
        };
        check(NativeBridge.open("http://localhost", "alice", "secret", "/not-used", false, listener) == 0);
        check(NativeBridge.open("https://localhost", "bad user", "secret", "/not-used", false, listener) == 0);
        check(NativeBridge.open("https://localhost", "alice", "", "/not-used", false, listener) == 0);
        System.out.println("Media framing, jitter, stereo Opus, and concealment passed");
    }
}
