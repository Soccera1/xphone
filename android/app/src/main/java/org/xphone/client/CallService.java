package org.xphone.client;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayDeque;

/** One serialized call state machine; Activity recreation never owns the connection. */
public final class CallService extends Service implements NativeBridge.Listener, CallAudio.Output {
    static final String CONNECT = "org.xphone.CONNECT", DISCONNECT = "org.xphone.DISCONNECT";
    interface Observer { void changed(View view); }
    static final class View {
        final String state, status, peer, transport;
        final long call;
        final boolean muted, speaker;
        final String[] users;
        final boolean[] busy;
        View(String state, String status, String peer, String transport, long call, boolean muted, boolean speaker,
             String[] users, boolean[] busy) {
            this.state = state; this.status = status; this.peer = peer; this.transport = transport;
            this.call = call; this.muted = muted; this.speaker = speaker; this.users = users; this.busy = busy;
        }
    }
    public final class LocalBinder extends Binder { CallService service() { return CallService.this; } }
    private final LocalBinder binder = new LocalBinder();
    private final Handler main = new Handler(Looper.getMainLooper());
    private HandlerThread thread;
    private Handler network;
    private AudioManager manager;
    private AudioFocusRequest focus;
    private AudioDeviceCallback devices;
    private PowerManager.WakeLock wake;
    private Ringtone ringtone;
    private volatile Observer observer;
    private volatile View view = new View("offline", "Disconnected", "", "", 0, false, false, new String[0], new boolean[0]);
    private String state = "offline", status = "Disconnected", peer = "", transport = "", username = "";
    private String[] users = new String[0];
    private boolean[] busy = new boolean[0];
    private long handle, call, lastRx, lastPing;
    private long lastWakeRenew;
    private int generation, tx, rx, advertisedCapture = 1;
    private boolean muted, stereo = true, speaker, mediaReady, foreground;
    private boolean communicationModeOwned;
    private String pendingFailure;
    private CallAudio audio;
    private long audioEpoch;
    private static final class Frame {
        final byte[] data;
        final long at = SystemClock.elapsedRealtime();
        Frame(byte[] data) { this.data = data; }
    }
    private final ArrayDeque<Frame> outgoing = new ArrayDeque<>();

    @Override public void onCreate() {
        super.onCreate();
        manager = getSystemService(AudioManager.class);
        thread = new HandlerThread("xphone-network"); thread.start(); network = new Handler(thread.getLooper());
        wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "xphone:call");
        wake.setReferenceCounted(false);
        getSystemService(NotificationManager.class).createNotificationChannel(
                new NotificationChannel("calls", "XPhone connection", NotificationManager.IMPORTANCE_DEFAULT));
        devices = new AudioDeviceCallback() {
            @Override public void onAudioDevicesAdded(AudioDeviceInfo[] added) { routeChanged(); }
            @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) { routeChanged(); }
        };
        manager.registerAudioDeviceCallback(devices, network);
    }

    @Override public IBinder onBind(Intent intent) { return binder; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) { stopSelf(); return START_NOT_STICKY; }
        if (DISCONNECT.equals(intent.getAction())) network.post(() -> disconnect("Disconnected"));
        else if (CONNECT.equals(intent.getAction())) {
            // Started from a visible Activity after RECORD_AUDIO permission has been granted.
            try {
                if (Build.VERSION.SDK_INT >= 30)
                    startForeground(1, notification("Connecting…"), ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
                else startForeground(1, notification("Connecting…"));
                foreground = true;
                network.post(this::connect);
            } catch (RuntimeException e) {
                network.post(() -> disconnect("Cannot start call service; open the app and grant microphone access"));
            }
        }
        return START_NOT_STICKY;
    }

    View snapshot() { return view; }
    void observe(Observer next) { observer = next; if (next != null) next.changed(view); }
    void removeObserver(Observer old) { if (observer == old) observer = null; }

    private Notification notification(String text) {
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, CallService.class).setAction(DISCONNECT),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, "calls").setSmallIcon(R.drawable.ic_phone)
                .setContentTitle("XPhone").setContentText(text).setContentIntent(open).setOngoing(true)
                .setCategory(Notification.CATEGORY_CALL).addAction(new Notification.Action.Builder(
                        null, "Disconnect", stop).build()).build();
    }

    private void publish() {
        View next = new View(state, status, peer, transport, call, muted, speaker, users.clone(), busy.clone());
        view = next;
        main.post(() -> {
            if (view != next) return;
            Observer current = observer;
            if (current != null) current.changed(next);
            if (foreground) getSystemService(NotificationManager.class).notify(1,
                    notification(next.peer.isEmpty() ? next.status : next.status + " · " + next.peer));
        });
    }

    private void connect() {
        if (handle != 0) return;
        state = "connecting"; status = "Connecting securely…"; publish();
        try {
            var prefs = getSharedPreferences("settings", 0);
            username = prefs.getString("user", "");
            stereo = prefs.getBoolean("stereo", true);
            advertisedCapture = stereo ? CallAudio.captureChannels(manager) : 1;
            handle = NativeBridge.open(prefs.getString("url", ""), username, Credentials.load(this),
                    TrustStore.export(this), prefs.getBoolean("http2", false), this);
            if (handle == 0) throw new IllegalStateException("Invalid connection settings or trust store");
            lastRx = lastPing = SystemClock.elapsedRealtime();
            network.post(tick);
        } catch (Exception e) { disconnect("Connection failed: check server, token, and certificates"); }
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (handle == 0) return;
            NativeBridge.pump(handle);
            if (pendingFailure != null) { String error = pendingFailure; pendingFailure = null; disconnect(error); return; }
            long now = SystemClock.elapsedRealtime();
            if (audio != null && now - lastWakeRenew >= 60000) {
                wake.acquire(10 * 60 * 1000L); lastWakeRenew = now;
            }
            if (now - lastRx > 45000) { disconnect("Connection timed out; reconnect to call again"); return; }
            if (!state.equals("connecting") && now - lastPing >= 15000) {
                send(json("type", "ping")); lastPing = now;
            }
            synchronized (outgoing) {
                while (!outgoing.isEmpty()) {
                    Frame frame = outgoing.removeFirst();
                    if (audio != null && now - frame.at < 200) NativeBridge.mediaSend(handle, frame.data);
                }
            }
            network.postDelayed(this, 1);
        }
    };

    private void send(JSONObject object) {
        if (handle != 0 && !NativeBridge.send(handle, object.toString())) pendingFailure = "Control connection failed";
    }

    private void hello() { send(json("type", "hello", "version", 1, "capture", advertisedCapture, "playback", stereo ? 2 : 1)); }

    @Override public void onControl(String text) {
        try {
            JSONObject event = new JSONObject(text);
            String type = event.getString("type");
            lastRx = SystemClock.elapsedRealtime();
            switch (type) {
                case "_transport": transport = event.getString("http") +
                        (event.optBoolean("datagrams") ? " · QUIC datagrams" : " · capsules"); break;
                case "_closed": pendingFailure = event.optString("reason", "Disconnected"); break;
                case "welcome":
                    if (event.getInt("version") != 1) throw new JSONException("Protocol version");
                    state = "online"; status = "Ready to call"; hello(); break;
                case "presence":
                    JSONArray list = event.getJSONArray("users");
                    if (list.length() > 128) throw new JSONException("Too many users");
                    java.util.ArrayList<String> names = new java.util.ArrayList<>();
                    java.util.ArrayList<Boolean> flags = new java.util.ArrayList<>();
                    for (int i = 0; i < list.length(); ++i) {
                        JSONObject entry = list.getJSONObject(i);
                        String name = entry.getString("name");
                        if (!name.matches("[A-Za-z0-9_-]{1,31}")) throw new JSONException("Invalid user");
                        if (!name.equals(username)) { names.add(name); flags.add(entry.getBoolean("busy")); }
                    }
                    users = names.toArray(new String[0]); busy = new boolean[users.length];
                    for (int i = 0; i < busy.length; ++i) busy[i] = flags.get(i);
                    break;
                case "incoming": case "ringing":
                    long incoming = event.getLong("call");
                    if (incoming <= 0 || (call != 0 && call != incoming)) throw new JSONException("Invalid call");
                    call = incoming; peer = event.getString("peer");
                    if (!peer.matches("[A-Za-z0-9_-]{1,31}")) throw new JSONException("Invalid peer");
                    state = type; status = type.equals("incoming") ? "Incoming call" : "Ringing…";
                    if (type.equals("incoming")) startRinging();
                    break;
                case "active":
                    if (event.getLong("call") != call || call <= 0) throw new JSONException("Unexpected active call");
                    int nextGeneration = event.getInt("generation");
                    int nextTx = event.getInt("tx_channels"), nextRx = event.getInt("rx_channels");
                    if (nextGeneration <= 0 || nextTx < 1 || nextTx > advertisedCapture ||
                            nextRx < 1 || nextRx > (stereo ? 2 : 1)) throw new JSONException("Invalid audio format");
                    stopRinging(); stopAudio(); generation = nextGeneration; tx = nextTx; rx = nextRx;
                    state = "active"; status = "Connecting audio…";
                    if (!NativeBridge.mediaOpen(handle, call)) { endLocal("Cannot open audio stream"); break; }
                    if (mediaReady) startAudio();
                    break;
                case "_media": mediaReady = true; if (state.equals("active")) startAudio(); break;
                case "_media_closed": if (call != 0) endLocal("Audio stream closed"); break;
                case "ended":
                    if (event.getLong("call") == call) { resetCall(); state = "online"; status = event.optString("reason", "Call ended"); }
                    break;
                case "error": status = event.optString("message", "Server error"); break;
                case "pong": break;
                default: throw new JSONException("Unknown event");
            }
            if (!type.equals("pong")) publish();
        } catch (JSONException | IllegalArgumentException e) { pendingFailure = "Invalid server message"; }
    }

    @Override public void onMedia(byte[] record) { if (audio != null) audio.receive(record); }

    void command(String action, String target, long expectedCall) {
        network.post(() -> {
            if (handle == 0) return;
            switch (action) {
                case "call":
                    if (state.equals("online") && target.matches("[A-Za-z0-9_-]{1,31}")) send(json("type", "call", "to", target));
                    break;
                case "accept":
                    if (call == expectedCall && state.equals("incoming")) { stopRinging(); send(json("type", "accept", "call", call)); }
                    break;
                case "end":
                    if (call != 0 && call == expectedCall) {
                        String type = state.equals("incoming") ? "reject" : state.equals("ringing") ? "cancel" : "hangup";
                        send(json("type", type, "call", call)); resetCall(); state = "online"; status = "Call ended";
                    }
                    break;
                case "mute": muted = !muted; if (audio != null) audio.mute(muted); break;
                case "speaker": speaker = !speaker; applyRoute(); break;
                default: break;
            }
            publish();
        });
    }

    private void startAudio() {
        if (audio != null || !mediaReady || call == 0) return;
        try {
            manager.setMode(AudioManager.MODE_IN_COMMUNICATION);
            communicationModeOwned = true;
            applyRoute();
            long epoch = audioEpoch;
            focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                    .setOnAudioFocusChangeListener(change -> {
                        if (epoch == audioEpoch && change < 0 && call != 0) endLocal("Call ended after losing audio focus");
                    }, network).build();
            if (manager.requestAudioFocus(focus) != AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
                throw new IllegalStateException("Audio focus unavailable");
            audio = new CallAudio(this, call, generation, tx, rx, muted, this);
            audio.start(); wake.acquire(10 * 60 * 1000L); lastWakeRenew = SystemClock.elapsedRealtime();
            status = "Connected · sending " + (tx == 2 ? "stereo" : "mono") + " · receiving " + (rx == 2 ? "stereo" : "mono");
        } catch (RuntimeException e) {
            if (tx == 2) {
                stopAudio(); advertisedCapture = 1; hello(); status = "Switching microphone to mono…";
            } else endLocal("Audio unavailable; check microphone permission and output device");
        }
    }

    private void applyRoute() {
        if (Build.VERSION.SDK_INT >= 31) {
            if (!speaker) manager.clearCommunicationDevice();
            else for (AudioDeviceInfo device : manager.getAvailableCommunicationDevices())
                if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) { manager.setCommunicationDevice(device); break; }
        } else manager.setSpeakerphoneOn(speaker);
    }

    private void routeChanged() {
        if (handle == 0 || state.equals("connecting")) return;
        int next = stereo ? CallAudio.captureChannels(manager) : 1;
        if (next == advertisedCapture) return;
        stopAudio(); advertisedCapture = next; hello();
    }

    private void stopAudio() {
        ++audioEpoch;
        if (audio != null) { audio.close(); audio = null; }
        synchronized (outgoing) { outgoing.clear(); }
        if (wake.isHeld()) wake.release();
        if (focus != null) { manager.abandonAudioFocusRequest(focus); focus = null; }
        if (communicationModeOwned) {
            if (Build.VERSION.SDK_INT >= 31) manager.clearCommunicationDevice();
            else manager.setSpeakerphoneOn(false);
            manager.setMode(AudioManager.MODE_NORMAL);
            communicationModeOwned = false;
        }
    }

    private void startRinging() {
        stopRinging();
        try {
            ringtone = RingtoneManager.getRingtone(this, RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE));
            if (ringtone != null) {
                if (Build.VERSION.SDK_INT >= 28) ringtone.setLooping(true);
                ringtone.play();
            }
        } catch (RuntimeException ignored) { }
    }

    private void stopRinging() { if (ringtone != null) { ringtone.stop(); ringtone = null; } }

    private void resetCall() {
        stopRinging(); stopAudio();
        if (handle != 0) NativeBridge.mediaClose(handle);
        call = 0; generation = 0; peer = ""; mediaReady = false; muted = false;
    }

    private void endLocal(String reason) {
        if (call != 0) send(json("type", "hangup", "call", call));
        resetCall(); state = "online"; status = reason; publish();
    }

    private void disconnect(String reason) {
        network.removeCallbacks(tick); resetCall();
        if (handle != 0) NativeBridge.close(handle);
        handle = 0; pendingFailure = null; state = "offline"; status = reason; transport = "";
        users = new String[0]; busy = new boolean[0]; publish();
        main.post(() -> { foreground = false; stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); });
    }

    @Override public void encoded(byte[] record) {
        synchronized (outgoing) {
            if (outgoing.size() == 10) outgoing.removeFirst();
            outgoing.addLast(new Frame(record));
        }
    }

    @Override public void failed(CallAudio source) {
        network.post(() -> { if (audio == source) endLocal("Audio device disconnected"); });
    }

    private static JSONObject json(Object... pairs) {
        JSONObject result = new JSONObject();
        try { for (int i = 0; i < pairs.length; i += 2) result.put((String) pairs[i], pairs[i + 1]); }
        catch (JSONException e) { throw new IllegalArgumentException(e); }
        return result;
    }

    @Override public void onDestroy() {
        observer = null;
        manager.unregisterAudioDeviceCallback(devices);
        network.post(() -> { disconnect("Disconnected"); thread.quitSafely(); });
        super.onDestroy();
    }
}
