package org.xphone.client;

import java.io.BufferedReader;
import java.io.InputStreamReader;

/** JSON-lines harness around the production JNI bridge, with synthetic stereo Opus. */
public final class BridgeHarness implements NativeBridge.Listener {
    private long handle, codec, call;
    private int generation, sequence;
    private long next;

    @Override public void onControl(String json) { System.out.println(json); }
    @Override public void onMedia(byte[] record) {
        if (call == 0) return; // already-buffered records after local hangup
        MediaPacket packet = MediaPacket.parse(record);
        if (packet == null || packet.call != call || packet.generation != generation) throw new AssertionError("Bad relayed media");
        short[] decoded = NativeBridge.decode(codec, packet.opus);
        if (decoded == null || decoded.length != 1920) throw new AssertionError("Bad Opus");
        System.out.println("{\"type\":\"received\",\"sequence\":" + packet.sequence + "}");
    }

    public static void main(String[] args) throws Exception { new BridgeHarness().run(args); }

    private void run(String[] args) throws Exception {
        handle = NativeBridge.open(args[0], args[1], args[2], args[3], Boolean.parseBoolean(args[4]), this);
        if (handle == 0) throw new AssertionError("Open failed");
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in));
        try {
            boolean running = true;
            while (running) {
                while (input.ready()) {
                    String line = input.readLine();
                    if (line == null || line.equals("QUIT")) { running = false; break; }
                    if (line.startsWith("MEDIA ")) {
                        String[] words = line.split(" "); call = Long.parseLong(words[1]); generation = Integer.parseInt(words[2]);
                        sequence = 0; next = System.nanoTime(); codec = NativeBridge.codecCreate(2, 2);
                        if (!NativeBridge.mediaOpen(handle, call)) throw new AssertionError("Media open failed");
                    } else if (line.equals("STOP")) {
                        call = 0; NativeBridge.mediaClose(handle);
                        if (codec != 0) { NativeBridge.codecDestroy(codec); codec = 0; }
                    } else if (!NativeBridge.send(handle, line)) throw new AssertionError("Send failed");
                }
                NativeBridge.pump(handle);
                if (call != 0 && System.nanoTime() >= next) {
                    short[] pcm = new short[1920];
                    for (int i = 0; i < 960; ++i) {
                        pcm[i * 2] = (short) (6000 * Math.sin((sequence * 960L + i) * 2 * Math.PI * 440 / 48000));
                        pcm[i * 2 + 1] = (short) (4000 * Math.sin((sequence * 960L + i) * 2 * Math.PI * 880 / 48000));
                    }
                    NativeBridge.mediaSend(handle, new MediaPacket(call, generation, sequence++, 2, NativeBridge.encode(codec, pcm)).pack());
                    next = System.nanoTime() + 20_000_000L;
                }
                Thread.sleep(1);
            }
        } finally {
            NativeBridge.close(handle);
            if (codec != 0) NativeBridge.codecDestroy(codec);
        }
    }
}
