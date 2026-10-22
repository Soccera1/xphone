# XPhone for Android

Native Java client for Android 8.0 (API 26) and later, on phones and tablets. It connects directly to the existing `xphoned` server and interoperates with the X11 client. There is no separate Android server or gateway.

The interface uses one scrollable column on phones and two independently scrollable panes when the current window is at least 600 dp wide, including tablet split-screen windows. Rotation preserves the service connection and call. Controls have large touch targets and work with Android accessibility services. The system volume keys control call volume during a call.

Java implements the interface, permissions, foreground service, call state, Android Keystore storage, certificate handling, AudioRecord capture, AudioTrack playback, and bounded jitter buffer. A small JNI bridge reuses this repository's C transport and libopus. This retains the same extended CONNECT protocol, HTTP/3 QUIC datagrams, certificate/hostname verification, and automatic HTTP/2 fallback as the desktop client. The app does not use a WebView or cleartext HTTP.

## Build

Requirements: Linux x86_64, JDK 17 or 21, Python 3.12+, CMake, Ninja, make, Perl, and the Android SDK. Install the pinned SDK components using Android Studio's SDK Manager or:

```sh
sdkmanager 'platforms;android-36' 'build-tools;35.0.0' 'cmake;3.22.1' 'ndk;28.2.13676358' 'platform-tools'
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/28.2.13676358"

# From the repository root; downloads and verifies the source lockfile, then
# builds static PIC dependencies for both supported ABIs. No system install.
python3 android/tools/build-native-deps.py
cd android
./gradlew assembleDebug lintDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The APK includes `arm64-v8a` for phones/tablets and `x86_64` for emulators and compatible devices. Native code is linked for 16 KiB page compatibility. The Gradle wrapper pins Gradle 8.13 and verifies its distribution checksum; the app pins AGP 8.13.2 and NDK r28c. `tools/sources.json` locks native source URLs and SHA-256 checksums, including nghttp3's sfparse submodule. Build downloads and intermediates are ignored by Git. Dependency license texts are bundled in the APK and accessible through **Third-party notices**.

To build dependencies for just one ABI, pass `--abi arm64-v8a` (or `--abi x86_64`), then build with `./gradlew assembleDebug -Pandroid.injected.build.abi=arm64-v8a`. Android Studio also supports selecting a device ABI. Remove `.native-build` and `native-deps` before changing dependency versions or the NDK. Release signing is intentionally left to the owner; `assembleRelease` produces an unsigned release APK.

## Connect and call

1. Provision an account with `xphoned add-user` as described in the root README.
2. Enter the `https://` server origin, username, and the 64-character token. The token is encrypted with a non-exportable Android Keystore AES-GCM key. App backup is disabled, and credentials are excluded from screenshots and saved Activity state.
3. For a private CA or development certificate, choose **Import CA** and select a PEM or DER certificate. Android's trusted CA certificates and this explicitly imported certificate are exported into the app's private trust bundle for OpenSSL. Importing a CA never disables hostname verification. Use a hostname present in the certificate; Android emulator host access commonly uses `10.0.2.2`, which must be covered by the certificate if used in the server URL.
4. Tap **Connect** and grant microphone permission. Notification permission is requested on Android 13+. Select an available person; the recipient opens the app from the notification and taps **Answer**. The microphone opens only after acceptance and the server authorizes the media stream. Decline, cancel, hang up, or disconnect releases capture immediately.
5. Use **Mute microphone**, the speaker/system-route toggle, and the device volume keys during a call. Headsets give the best echo isolation. Enable stereo to negotiate up to two channels in each direction; unsupported capture falls back to mono. Output follows Android communication routing; some device routes are inherently mono. Audio device changes renegotiate capabilities.

The foreground service keeps the connection and active call independent of Activity lifetime. An active call holds a partial wake lock. Audio-focus loss ends the call, and process death or a network failure does not automatically resume microphone capture. The app does not start at boot, integrate with the cellular dialer, provide push wakeups, or automatically reconnect. Idle availability depends on Android's network/battery policies; open the app and reconnect if the session expires. Disconnect from the app or persistent notification to stop the service. Permission revocation and audio-device failures stop capture rather than silently continuing a call.

The server is trusted with the compressed audio, as with the X11 client; HTTPS provides transport encryption, not end-to-end encryption.

## Validation

With the regular native dependencies and desktop binaries built, run:

```sh
export JAVA_HOME=/path/to/jdk-17-or-21
export PATH="$JAVA_HOME/bin:$PATH"
sh android/tools/test-host.sh
```

This compiles the production JNI bridge on the host, tests media framing, sequence wraparound, jitter reordering and concealment, true stereo Opus, and exercises Java-to-desktop calls against a real local TLS server over HTTP/3, HTTP/2, and UDP-blocked fallback. It also checks repeated calls beyond QUIC's initial stream allowance, authentication rejection, duplicate login, and hostname verification. All audio in these tests is synthetic; the user's microphone is untouched.

`./gradlew lintDebug` checks Android API compatibility and resources. Before distribution, test on physical phones and tablets: portrait/landscape and split-screen, permission denial/revocation, background/locked-screen calls, Bluetooth/wired/USB routing, device removal, acoustic echo, stereo capture, network changes, and manufacturer battery restrictions. Host tests cannot verify these device-specific behaviors.
