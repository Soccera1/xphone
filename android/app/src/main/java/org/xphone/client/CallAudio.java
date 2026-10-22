package org.xphone.client;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.os.Process;
import android.os.SystemClock;
import java.util.Arrays;

/** AudioRecord/AudioTrack own their worker threads. Construct only after media HTTP 200. */
final class CallAudio implements AutoCloseable {
    interface Output { void encoded(byte[] record); void failed(CallAudio source); }
    private final AudioRecord recorder;
    private final AudioTrack player;
    private final AcousticEchoCanceler echo;
    private final long codec, call;
    private final int generation, tx, rx;
    private final Output output;
    private final JitterBuffer jitter = new JitterBuffer();
    private Thread capture, playback;
    private volatile boolean running, muted;
    private boolean closed;

    static int captureChannels(AudioManager manager) {
        for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            if (device.getType() != AudioDeviceInfo.TYPE_BUILTIN_MIC && device.getType() != AudioDeviceInfo.TYPE_TELEPHONY) {
                for (int count : device.getChannelCounts()) if (count >= 2) return 2;
                return 1;
            }
        }
        for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            // Prefer a conservative mono advertisement for headsets/unknown devices.
            if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_MIC)
                for (int count : device.getChannelCounts()) if (count >= 2) return 2;
        }
        return 1;
    }

    CallAudio(Context context, long call, int generation, int tx, int rx, boolean muted, Output output) {
        if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("Microphone permission required");
        this.call = call; this.generation = generation; this.tx = tx; this.rx = rx;
        this.muted = muted; this.output = output;
        int inputMask = tx == 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
        int outputMask = rx == 2 ? AudioFormat.CHANNEL_OUT_STEREO : AudioFormat.CHANNEL_OUT_MONO;
        int inputMin = AudioRecord.getMinBufferSize(48000, inputMask, AudioFormat.ENCODING_PCM_16BIT);
        int outputMin = AudioTrack.getMinBufferSize(48000, outputMask, AudioFormat.ENCODING_PCM_16BIT);
        if (inputMin <= 0 || outputMin <= 0) throw new IllegalStateException("Audio format unavailable");
        AudioRecord input = null;
        AudioTrack track = null;
        long nativeCodec = 0;
        AcousticEchoCanceler effect = null;
        try {
            input = new AudioRecord.Builder().setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
                    .setAudioFormat(format(inputMask)).setBufferSizeInBytes(Math.max(inputMin, 960 * tx * 4)).build();
            track = new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                    .setAudioFormat(format(outputMask)).setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(Math.max(outputMin, 960 * rx * 4)).build();
            if (input.getState() != AudioRecord.STATE_INITIALIZED || track.getState() != AudioTrack.STATE_INITIALIZED)
                throw new IllegalStateException("Audio device unavailable");
            nativeCodec = NativeBridge.codecCreate(tx, rx);
            if (nativeCodec == 0) throw new IllegalStateException("Opus unavailable");
            if (AcousticEchoCanceler.isAvailable()) {
                effect = AcousticEchoCanceler.create(input.getAudioSessionId());
                if (effect != null) effect.setEnabled(true);
            }
            recorder = input; player = track; codec = nativeCodec; echo = effect;
        } catch (RuntimeException e) {
            if (effect != null) effect.release();
            if (input != null) input.release();
            if (track != null) track.release();
            if (nativeCodec != 0) NativeBridge.codecDestroy(nativeCodec);
            throw e;
        }
    }

    private static AudioFormat format(int mask) {
        return new AudioFormat.Builder().setSampleRate(48000).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(mask).build();
    }

    void start() {
        try {
            recorder.startRecording();
            if (recorder.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING)
                throw new IllegalStateException("Microphone unavailable");
            player.play();
            running = true;
            capture = new Thread(this::captureLoop, "xphone-capture");
            playback = new Thread(this::playbackLoop, "xphone-playback");
            capture.start(); playback.start();
        } catch (RuntimeException e) { close(); throw e; }
    }

    void mute(boolean value) { muted = value; }

    void receive(byte[] record) {
        MediaPacket p = MediaPacket.parse(record);
        if (p != null && p.call == call && p.generation == generation && p.channels == rx)
            jitter.offer(p.sequence, p.opus, SystemClock.elapsedRealtime());
    }

    private void captureLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        short[] pcm = new short[960 * tx];
        int sequence = 0;
        try {
            while (running) {
                int at = 0;
                while (running && at < pcm.length) {
                    int n = recorder.read(pcm, at, pcm.length - at, AudioRecord.READ_BLOCKING);
                    if (n <= 0) throw new IllegalStateException("Microphone read failed");
                    at += n;
                }
                if (!running) break;
                if (muted) Arrays.fill(pcm, (short) 0);
                byte[] packet = NativeBridge.encode(codec, pcm);
                if (packet == null) throw new IllegalStateException("Opus encoding failed");
                output.encoded(new MediaPacket(call, generation, sequence++, tx, packet).pack());
            }
        } catch (RuntimeException e) { if (running) output.failed(this); }
    }

    private void playbackLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        try {
            while (running && !jitter.ready(SystemClock.elapsedRealtime())) Thread.sleep(5);
            long next = System.nanoTime();
            while (running) {
                short[] pcm = NativeBridge.decode(codec, jitter.take());
                if (pcm == null) throw new IllegalStateException("Opus decoding failed");
                int at = 0;
                while (running && at < pcm.length) {
                    int n = player.write(pcm, at, pcm.length - at, AudioTrack.WRITE_BLOCKING);
                    if (n <= 0) throw new IllegalStateException("Speaker write failed");
                    at += n;
                }
                next += 20_000_000L;
                long delay = next - System.nanoTime();
                if (delay > 0) Thread.sleep(delay / 1_000_000L, (int) (delay % 1_000_000L));
                if (delay < -200_000_000L) next = System.nanoTime();
            }
        } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        catch (RuntimeException e) { if (running) output.failed(this); }
    }

    @Override public synchronized void close() {
        if (closed) return;
        closed = true;
        running = false;
        try { recorder.stop(); } catch (IllegalStateException ignored) { }
        try { player.pause(); player.flush(); player.stop(); } catch (IllegalStateException ignored) { }
        if (capture != null) capture.interrupt();
        if (playback != null) playback.interrupt();
        join(capture); join(playback);
        if (echo != null) echo.release();
        recorder.release(); player.release();
        NativeBridge.codecDestroy(codec);
    }

    private static void join(Thread thread) {
        if (thread == null) return;
        boolean interrupted = false;
        for (;;) {
            try { thread.join(); break; }
            catch (InterruptedException e) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }
}
