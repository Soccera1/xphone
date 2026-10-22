package org.xphone.client;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.text.InputType;
import android.util.AtomicFile;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.security.cert.CertificateFactory;

/** Platform Java views: large touch targets, scrollable phone UI, tablet two-pane UI. */
public final class MainActivity extends Activity implements CallService.Observer {
    private static final int PERMISSIONS = 1, CERTIFICATE = 2;
    private CallService service;
    private boolean bound;
    private EditText url, user, token;
    private CheckBox stereo, http2;
    private LinearLayout settings, contacts, certificateControls;
    private TextView connectionStatus, callTitle, callStatus, connectionDetails, empty, caStatus;
    private Button connect, disconnect, accept, end, mute, route;
    private String contactSignature = "";
    private CallService.View current;

    private final ServiceConnection binding = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            service = ((CallService.LocalBinder) binder).service(); service.observe(MainActivity.this);
        }
        @Override public void onServiceDisconnected(ComponentName name) { service = null; }
    };

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        LinearLayout root = column(); root.setPadding(dp(20), dp(12), dp(20), dp(12));
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(dp(20) + insets.getSystemWindowInsetLeft(), dp(12) + insets.getSystemWindowInsetTop(),
                    dp(20) + insets.getSystemWindowInsetRight(), dp(12) + insets.getSystemWindowInsetBottom());
            return insets;
        });
        root.addView(label("XPhone", 30, true));
        root.addView(label("Private voice calls", 16, false));
        connectionStatus = label("Disconnected", 16, false); root.addView(connectionStatus);

        LinearLayout people = column();
        settings = column(); people.addView(settings);
        settings.addView(label("Connect to your server", 21, true));
        url = field("Server URL", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        user = field("Username", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        token = field("Access token", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        token.setSaveEnabled(false);
        var prefs = getSharedPreferences("settings", 0);
        url.setText(prefs.getString("url", "")); user.setText(prefs.getString("user", ""));
        token.setHint(getSharedPreferences("credentials", 0).contains("data") ? "Token stored securely (leave blank to keep)" : "Access token");
        settings.addView(url); settings.addView(user); settings.addView(token);
        stereo = new CheckBox(this); stereo.setText(R.string.stereo_option); stereo.setChecked(prefs.getBoolean("stereo", true));
        http2 = new CheckBox(this); http2.setText(R.string.http2_option); http2.setChecked(prefs.getBoolean("http2", false));
        settings.addView(stereo); settings.addView(http2);
        caStatus = label("", 14, false); settings.addView(caStatus); updateCa();
        certificateControls = row();
        certificateControls.addView(button("Import CA", v -> importCa()), weighted());
        certificateControls.addView(button("Remove CA", v -> {
            if (TrustStore.custom(this).exists() && !TrustStore.custom(this).delete()) toast("Cannot remove certificate");
            updateCa();
        }), weighted());
        settings.addView(certificateControls);
        settings.addView(button("Third-party notices", v -> showNotices()));
        connect = button("Connect", v -> requestConnection()); settings.addView(connect);
        disconnect = button("Disconnect", v -> startService(new Intent(this, CallService.class).setAction(CallService.DISCONNECT)));
        people.addView(disconnect);
        people.addView(label("People online", 21, true));
        empty = label("Connect to see available people.", 16, false); people.addView(empty);
        contacts = column(); people.addView(contacts);

        LinearLayout callPane = column(); callPane.setPadding(wide ? dp(24) : 0, dp(20), 0, 0);
        callTitle = label("Ready when you are", 28, true); callPane.addView(callTitle);
        callStatus = label("Choose someone online to start a call.", 18, false); callPane.addView(callStatus);
        connectionDetails = label("", 14, false); callPane.addView(connectionDetails);
        accept = button("Answer", v -> command("accept")); callPane.addView(accept);
        end = button("End call", v -> command("end")); callPane.addView(end);
        mute = button("Mute microphone", v -> command("mute")); callPane.addView(mute);
        route = button("Toggle speaker / system route", v -> command("speaker")); callPane.addView(route);
        callPane.addView(label("Microphone opens only after a call is accepted. Stereo depends on the devices at both ends. Use a headset for the best echo isolation.", 14, false));

        if (wide) {
            LinearLayout panes = row();
            panes.addView(scroll(people), new LinearLayout.LayoutParams(0, -1, 1));
            panes.addView(scroll(callPane), new LinearLayout.LayoutParams(0, -1, 1.2f));
            root.addView(panes, new LinearLayout.LayoutParams(-1, 0, 1));
        } else {
            LinearLayout content = column(); content.addView(people); content.addView(callPane);
            root.addView(scroll(content), new LinearLayout.LayoutParams(-1, 0, 1));
        }
        setContentView(root); root.requestApplyInsets();
        changed(new CallService.View("offline", "Disconnected", "", "", 0, false, false, new String[0], new boolean[0]));
    }

    @Override protected void onStart() {
        super.onStart();
        bound = bindService(new Intent(this, CallService.class), binding, Context.BIND_AUTO_CREATE);
    }

    @Override protected void onStop() {
        if (service != null) service.removeObserver(this);
        if (bound) unbindService(binding);
        bound = false; service = null;
        super.onStop();
    }

    private void requestConnection() {
        String server = url.getText().toString().trim(), name = user.getText().toString().trim();
        try {
            java.net.URI uri = new java.net.URI(server);
            if (!"https".equals(uri.getScheme()) || uri.getHost() == null || uri.getRawUserInfo() != null ||
                    uri.getRawQuery() != null || uri.getRawFragment() != null ||
                    (uri.getPort() != -1 && (uri.getPort() < 1 || uri.getPort() > 65535)) ||
                    (uri.getPath() != null && !uri.getPath().isEmpty() && !uri.getPath().equals("/")))
                throw new IllegalArgumentException();
            if (!name.matches("[A-Za-z0-9_-]{1,31}")) { toast("Enter a valid username"); return; }
            String secret = token.getText().toString().trim();
            if (!secret.isEmpty()) {
                if (!secret.matches("[0-9a-fA-F]{64}")) { toast("The access token must contain 64 hexadecimal characters"); return; }
                Credentials.save(this, secret);
            } else if (Credentials.load(this).isEmpty()) { toast("Enter your access token"); return; }
            getSharedPreferences("settings", 0).edit().putString("url", server).putString("user", name)
                    .putBoolean("stereo", stereo.isChecked()).putBoolean("http2", http2.isChecked()).apply();
            token.setText(""); token.setHint("Token stored securely (leave blank to keep)");
        } catch (java.net.URISyntaxException | IllegalArgumentException e) { toast("Enter an HTTPS server URL, for example https://phone.example.org"); return; }
        catch (Exception e) { toast("Cannot access secure token storage; enter your token again"); return; }

        java.util.ArrayList<String> permissions = new java.util.ArrayList<>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            permissions.add(Manifest.permission.RECORD_AUDIO);
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            permissions.add(Manifest.permission.POST_NOTIFICATIONS);
        if (!permissions.isEmpty()) requestPermissions(permissions.toArray(new String[0]), PERMISSIONS);
        else connectService();
    }

    private void connectService() { startForegroundService(new Intent(this, CallService.class).setAction(CallService.CONNECT)); }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(request, permissions, grants);
        if (request == PERMISSIONS) {
            if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) connectService();
            else toast("Microphone permission is required to make calls");
        }
    }

    private void command(String action) { if (service != null && current != null) service.command(action, "", current.call); }

    @Override public void changed(CallService.View view) {
        current = view;
        boolean offline = view.state.equals("offline"), active = view.state.equals("active"), incoming = view.state.equals("incoming");
        setVolumeControlStream(active ? android.media.AudioManager.STREAM_VOICE_CALL : android.media.AudioManager.STREAM_MUSIC);
        connectionStatus.setText(view.status);
        settings.setVisibility(offline ? View.VISIBLE : View.GONE);
        disconnect.setVisibility(offline ? View.GONE : View.VISIBLE);
        callTitle.setText(view.call == 0 ? "Ready when you are" : view.peer);
        callStatus.setText(view.call == 0 ? (offline ? "Connect to your server to begin." : "Choose someone online to start a call.") : view.status);
        connectionDetails.setText(view.transport);
        accept.setVisibility(incoming ? View.VISIBLE : View.GONE);
        end.setVisibility(view.call != 0 ? View.VISIBLE : View.GONE);
        end.setText(incoming ? "Decline" : view.state.equals("ringing") ? "Cancel call" : "End call");
        mute.setVisibility(active ? View.VISIBLE : View.GONE); mute.setText(view.muted ? "Unmute microphone" : "Mute microphone");
        route.setVisibility(active ? View.VISIBLE : View.GONE);
        route.setText(view.speaker ? "Use system audio route" : "Use speaker");
        empty.setVisibility(view.users.length == 0 ? View.VISIBLE : View.GONE);
        empty.setText(offline ? "Connect to see available people." : "No one else is online yet.");
        String signature = view.state + java.util.Arrays.toString(view.users) + java.util.Arrays.toString(view.busy);
        if (!signature.equals(contactSignature)) {
            contactSignature = signature; contacts.removeAllViews();
            for (int i = 0; i < view.users.length; ++i) {
                String name = view.users[i];
                Button person = button(name + (view.busy[i] ? " · Busy" : " · Call"), v -> {
                    if (service != null) service.command("call", name, 0);
                });
                person.setEnabled(view.state.equals("online") && !view.busy[i]);
                contacts.addView(person);
            }
        }
    }

    private void importCa() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                .addCategory(Intent.CATEGORY_OPENABLE), CERTIFICATE);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != CERTIFICATE || result != RESULT_OK || data == null || data.getData() == null) return;
        if (current != null && !current.state.equals("offline")) { toast("Disconnect before changing certificates"); return; }
        AtomicFile file = new AtomicFile(TrustStore.custom(this));
        FileOutputStream out = null;
        try (var input = getContentResolver().openInputStream(data.getData())) {
            if (input == null) throw new IllegalArgumentException();
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096]; int n;
            while ((n = input.read(buffer)) != -1) {
                if (bytes.size() + n > 262144) throw new IllegalArgumentException();
                bytes.write(buffer, 0, n);
            }
            var certificates = CertificateFactory.getInstance("X.509")
                    .generateCertificates(new java.io.ByteArrayInputStream(bytes.toByteArray()));
            if (certificates.isEmpty()) throw new IllegalArgumentException();
            out = file.startWrite(); out.write(bytes.toByteArray()); file.finishWrite(out); out = null;
            updateCa(); toast("Certificate imported; hostname verification remains enabled");
        } catch (Exception e) { if (out != null) file.failWrite(out); toast("Choose a valid PEM or DER X.509 certificate (at most 256 KiB)"); }
    }

    private void updateCa() { caStatus.setText(TrustStore.custom(this).exists() ? "Trust: Android certificates + imported CA" : "Trust: Android certificates"); }
    private void showNotices() {
        StringBuilder text = new StringBuilder("XPhone uses OpenSSL, libevent, ngtcp2, nghttp3, nghttp2, Jansson, Opus, and sfparse.\n\n");
        try {
            String[] files = getAssets().list("third-party");
            if (files != null) {
                java.util.Arrays.sort(files);
                for (String name : files) try (var input = getAssets().open("third-party/" + name)) {
                    text.append(name).append("\n\n");
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096]; int count;
                    while ((count = input.read(buffer)) != -1) bytes.write(buffer, 0, count);
                    text.append(new String(bytes.toByteArray(), java.nio.charset.StandardCharsets.UTF_8)).append("\n\n");
                }
            }
        } catch (java.io.IOException e) { toast("Cannot read third-party notices"); return; }
        TextView content = label(text.toString(), 14, false);
        content.setTextIsSelectable(true); content.setPadding(dp(16), dp(16), dp(16), dp(16));
        new android.app.AlertDialog.Builder(this).setTitle("Third-party notices").setView(scroll(content))
                .setPositiveButton("Close", null).show();
    }
    private void toast(String text) { Toast.makeText(this, text, Toast.LENGTH_LONG).show(); }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private LinearLayout column() { LinearLayout v = new LinearLayout(this); v.setOrientation(LinearLayout.VERTICAL); return v; }
    private LinearLayout row() { LinearLayout v = new LinearLayout(this); v.setOrientation(LinearLayout.HORIZONTAL); return v; }
    private LinearLayout.LayoutParams weighted() { return new LinearLayout.LayoutParams(0, -2, 1); }
    private ScrollView scroll(View content) { ScrollView v = new ScrollView(this); v.setFillViewport(true); v.addView(content); return v; }
    private TextView label(String text, int size, boolean bold) {
        TextView v = new TextView(this); v.setText(text); v.setTextSize(size); v.setTextColor(Color.rgb(25, 48, 44));
        v.setPadding(0, dp(8), 0, dp(8)); if (bold) v.setTypeface(null, android.graphics.Typeface.BOLD); return v;
    }
    private EditText field(String hint, int type) {
        EditText v = new EditText(this); v.setHint(hint); v.setContentDescription(hint); v.setInputType(type);
        v.setSingleLine(true); v.setMinHeight(dp(56)); v.setLayoutParams(new ViewGroup.LayoutParams(-1, -2)); return v;
    }
    private Button button(String text, View.OnClickListener click) {
        Button v = new Button(this); v.setText(text); v.setAllCaps(false); v.setMinHeight(dp(52));
        v.setLayoutParams(new LinearLayout.LayoutParams(-1, -2)); v.setOnClickListener(click); return v;
    }
}
