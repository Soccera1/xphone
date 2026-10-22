# XPhone

A native C/Xlib voice phone for **Linux phones and desktop PCs**, with an invited-user relay server. Calls support real stereo, speakerphone echo cancellation, presence, incoming ringing, mute, and volume control.

All traffic uses HTTPS. HTTP/3 over QUIC is preferred; HTTP/2 over TLS is the fallback. HTTP/3 carries audio in HTTP Datagrams and keeps signaling reliable. The server bridges clients on different transports without transcoding audio. The server operator is trusted with call audio.

## Build

Use a C17 compiler, CMake 3.20+, pkg-config, and development packages for:

- OpenSSL **3.5+**, ngtcp2 **1.22+** with `libngtcp2_crypto_ossl`, nghttp3 **1.14+**, nghttp2, libevent, and Jansson.
- Client: X11, Xft, XInput2, PulseAudio, Opus, and SpeexDSP.

PipeWire works through its PulseAudio compatibility service. The server does not link the client's X11/audio libraries. To configure a server-only build without client development packages, use `-DXPHONE_BUILD_CLIENT=OFF`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The HTTP libraries are required at configure time, so HTTP/3 support cannot silently disappear from a build. Native ARM64 and x86-64 builds use the same commands. For cross-compilation, use [the ARM64 toolchain](cmake/aarch64-linux.cmake) with a target sysroot and target pkg-config search paths; host libraries must not be used for target linking.

## Run a server

Get a TLS certificate for your server hostname, then create invitations:

```sh
mkdir -m 700 server-config
build/xphoned add-user alice --users server-config/users.json
build/xphoned add-user bob --users server-config/users.json
```

Each command prints a JSON object containing a username and a **one-time displayed secret token**. Give each person their own credentials through a private channel. Only SHA-256 token hashes are retained in the users file. Tokens are 256 random bits. Duplicate usernames are rejected; `add-user` serializes concurrent updates and atomically saves the file.

Copy [examples/server.json](examples/server.json), change the paths, bind address, and port, then run:

```sh
build/xphoned --config server-config/server.json
```

Open **TCP 443 and UDP 443** at the server firewall. Clients require outbound access only. An unprivileged local setup can use a higher port such as 8443. The default bind address serves IPv4; use `::` for the host's IPv6/dual-stack listener.

The server loads invitations at startup; restart it after adding or removing users. Removing a user's entry and restarting revokes that token. Generate replacement invitations with a new username, or remove the old entry before issuing a replacement for the same name.

The [systemd service](packaging/xphoned.service) runs as a dedicated `xphone` user with permission to bind port 443. Create that system account, put configuration/certificates in `/etc/xphone`, make the users file owned by `xphone` with mode `0600`, and ensure the service user can read its TLS private key. Adjust `ExecStart` if installing somewhere other than `/usr/local/bin`. Point `certificate` at the full certificate chain, and restart the service after certificate renewal.

For OpenRC, use the [init script](packaging/xphoned.initd) and [conf.d settings](packaging/xphoned.confd). After `cmake --install build`, the files are available under `${prefix}/share/xphone/openrc`. With the default `/usr/local` prefix, install and enable them as root:

```sh
install -m 0755 /usr/local/share/xphone/openrc/init.d/xphoned /etc/init.d/xphoned
install -m 0644 /usr/local/share/xphone/openrc/conf.d/xphoned /etc/conf.d/xphoned
rc-update add xphoned default
rc-service xphoned start
```

Create the same dedicated `xphone` user/group and prepare `/etc/xphone` and certificate permissions as above before starting the service. Edit `/etc/conf.d/xphoned` to change the binary/configuration paths, service account, or log directory; preserve existing local settings when updating that file. As with systemd, the users file must be owned by the service user with mode `0600`.

The OpenRC service uses `supervise-daemon`, retries crashes after three seconds (at most five respawns within 60 seconds), and writes logs to `/var/log/xphone/xphoned.log`. Its `start_pre` hook creates the log directory with the service account as owner. OpenRC must be built with libcap support to grant the ambient `CAP_NET_BIND_SERVICE` capability for TCP/UDP 443. For an unprivileged server port, set `xphoned_capabilities=""` in `conf.d`; the service still runs as `xphone`. Use `rc-service xphoned restart` after invitation or certificate changes, and `rc-service xphoned stop` to stop it.

HTTPS terminates directly in `xphoned`. A normal HTTP reverse proxy is insufficient for the QUIC media path: a proxy must explicitly support the XPhone extended-CONNECT protocols and RFC 9297 datagram forwarding. HTTP/1.1 and WebSocket fallback are not implemented.

## Run a client

Copy [examples/client.json](examples/client.json) to `~/.config/xphone/client.json`, fill in your server URL and invitation, and restrict its permissions:

```sh
chmod 600 ~/.config/xphone/client.json
build/xphone
# Or choose a different configuration:
build/xphone --config /path/to/client.json
```

The default location honors `XDG_CONFIG_HOME`. Credential files must be regular files owned by the current user with no group/other permissions; symbolic links are rejected.

Select a person and press **Call selected person**. Answer or decline incoming calls. During a call, use **Mute mic**, the volume buttons, or **Hang up**. Tab/Shift-Tab move keyboard focus, Enter/Space activate controls, arrow keys select people, **M** toggles mute, and Escape declines/cancels a ringing call. Touch supports taps and dragging the contacts list; scrolling does not place calls.

Phones use a compact contacts/call view; wider windows show both side by side. Controls have at least 48 logical-pixel touch targets. Portrait 320×480 and landscape 480×320 logical layouts are supported. Fonts and controls scale together from `Xft.dpi`; set `"ui_scale": 2` (or another value between 0.5 and 4) if your mobile X11 environment does not publish useful DPI information. Scaling is read when the client starts.

Optional client settings:

| Setting | Default | Behavior |
| --- | --- | --- |
| `audio_channels` | `"auto"` | Stereo when capture and remote playback support it; otherwise mono. `"mono"` saves bandwidth. `"stereo"` requires stereo-capable local devices. |
| `opus_complexity` | `5` | Integer 0–10; lower values use less CPU. |
| `transport` | `"auto"` | Try HTTP/3 first; fall back to HTTP/2 after two seconds. `"http2"` forces TCP. |
| `ca_file` | system trust | PEM CA certificate for a private/test certificate authority. Hostname verification remains enabled. |
| `ui_scale` | Xft DPI / 96 | Explicit display scale. |

Audio uses the system's default input/output devices. Use your desktop's audio settings to select a headset, speakers, or another source. Hardware with one microphone remains mono even if the output has two speakers. Each direction negotiates separately, preserving actual left/right channels. Calls use 48 kHz/20 ms Opus frames at about 32 kbit/s mono or 64 kbit/s stereo, plus transport overhead.

Speakerphone processing uses multichannel SpeexDSP cancellation with a playback reference and measured stream latency. Performance depends on hardware delay, clock drift, volume, and room acoustics. Default device moves trigger discovery and renegotiation; stream/device failures end the call with an audio error. The microphone opens only after a call is accepted. The client releases audio streams after hangup, and incoming ringing uses output only.

HTTP/2 calls can suffer added latency during packet loss because TCP retransmits. Queues are bounded, old queued media is discarded, and the receiver rejects frames that miss their playout position. Full connection loss ends the call and releases capture. Presence reconnects automatically; calls do not automatically resume after a disconnect or suspend. The application does not override the phone's suspend policy.

## Verification and troubleshooting

Tests start actual local C servers/clients with temporary TLS certificates. They cover HTTP/3, HTTP/2, mixed transports, QUIC-blocked fallback, mono/stereo, mute, rejection, repeated calls beyond initial QUIC stream credit, busy/offline/self calls, the real 30-second ringing timeout, disconnect cleanup, invalid tokens, invalid trust, and hostname mismatches. A user-space UDP relay injects packet loss, reordering, and ciphertext corruption; another test stalls an HTTP/2 receiver. Audio tests check channel separation, silent mute, bounded playout, and synthetic multichannel echo reduction.

When PulseAudio, pactl, and dbus-run-session are available, integration tests create an isolated audio daemon with generated stereo input and virtual sinks. They verify real capture/playback streams, live stereo-to-mono device renegotiation, and microphone release before/after calls. They do not change the user's audio daemon or use physical microphones/speakers. `XPHONE_TEST_ONLY_AUDIO=1` selects just this audio integration scenario. Tests also print desktop CPU/RSS samples; these are diagnostics rather than hardware-independent benchmarks.

When Xvfb, xdotool, and ImageMagick are installed, GUI tests exercise live calls, contact dragging, keyboard/mouse controls, resizing, and 1×/2× scaling. Screenshots are saved under `build/screenshots`. The full integration test takes about a minute because it verifies the actual ringing timeout.

```sh
cmake -S . -B build-sanitize -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DXPHONE_SANITIZE=ON
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

Clang's sanitizer runtime must be installed. For a terminal diagnostic session:

```sh
build/xphone --config client.json --headless
```

It prints JSON events and accepts newline-delimited commands such as `{"action":"call","target":"bob"}`, `{"action":"accept"}`, and `{"action":"hangup"}` on standard input. `--synthetic` explicitly replaces physical input/output with generated stereo tones for automated tests. `--auto-answer`, `--call USER`, `--duration SECONDS`, `--quit-after SECONDS`, and `--geometry WIDTHxHEIGHT` support scripted verification.

If the client cannot connect, check the URL, certificate hostname, CA trust, invitation, firewall, and whether the username is already connected. Certificate/authentication failures never disable verification or retry indefinitely. If HTTP/3 is unavailable, the status shows HTTP/2 after fallback. Audio errors usually indicate that PulseAudio/PipeWire is unavailable or the selected device disappeared.

Real ARM64 phone battery/CPU measurements, physical touchscreen input, and room-acoustic speakerphone quality require target hardware. Automated desktop and virtual-device tests do not substitute for those acceptance checks. Native Android/iOS clients, public signup, text/video/group calls, SIP/PSTN, recording, and end-to-end encryption are outside this version.
