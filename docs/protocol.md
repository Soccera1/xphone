# XPhone protocol v1

The native client and server communicate over HTTPS with ALPN `h3` (QUIC v1) or `h2` (TCP), TLS 1.3, and normal certificate/hostname verification. No 0-RTT application data is accepted. The server enables extended CONNECT; HTTP/3 peers also negotiate HTTP Datagrams and QUIC DATAGRAM support.

## Authentication and control

Open extended CONNECT `/v1/session` with `:protocol: xphone-control-v1`, `:scheme: https`, `:authority`, `x-xphone-user: USER`, and `authorization: Bearer TOKEN`. Credentials are never indexed in QPACK/HPACK. HTTP 200 opens a persistent duplex control stream. HTTP 401 rejects credentials, 409 rejects a duplicate login, and 501 rejects an unsupported CONNECT protocol. Header sections are limited to 8 KiB.

The control stream carries UTF-8 JSON objects prefixed by an unsigned 32-bit network-order byte length, from 1 to 65536 inclusive. Duplicate JSON keys and malformed frames are rejected.

Server events:

- `welcome`: `version:1`, `user`.
- `presence`: `users:[{name,busy}]` containing online users.
- `ringing` / `incoming`: `call` and `peer`.
- `active`: `call`, `peer`, positive `generation`, `tx_channels`, `rx_channels`.
- `ended`: `call`, human-readable `reason`.
- `pong`, or `error` with a human-readable `message`.

Client messages:

- `hello`: `version:1`, `capture:1|2`, `playback:1|2`. Required before placing calls; repeated after a device change.
- `call`: `to:USERNAME`.
- `accept`, `reject`, `cancel`, `hangup`: `call:CALL_ID`.
- `ping`: sent every 15 seconds; a control connection expires after 45 seconds without a valid message.

Usernames contain 1–31 ASCII letters/digits, underscores, or hyphens. Call IDs are random positive signed 63-bit JSON integers. One pending/active call is permitted per user. The first processed valid invitation wins conflicts; pending calls expire after 30 seconds. Only the recipient can accept. Disconnecting either participant ends the call. Repeated or stale `hangup`/`cancel` messages are harmless and cannot affect a different call.

Each direction uses the minimum of the sender's capture channels and recipient's playback channels. A capability change increments the generation and sends updated `active` events to both users; both sides reset codec/playout state. The relay never transcodes.

## Media

For an accepted call, open extended CONNECT `/v1/calls/CALL_ID/media` on the same connection with `:protocol: xphone-media-v1`, the same authentication headers, and `capsule-protocol: ?1`. It must belong to that user's active call. One media stream per participant is allowed.

On HTTP/3 with negotiated DATAGRAM support, use RFC 9297 HTTP Datagrams: prepend the request stream ID divided by four as a QUIC variable-length integer, then the media record. Otherwise carry records in RFC 9297 DATAGRAM capsules (type `0x00`, QUIC-varint length, media record). HTTP/2 always uses capsules. Unknown bounded capsule types are skipped. The application record is at most 1100 bytes, including its 24-byte header:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | Version `1` |
| 1 | 1 | Channel count `1` or `2` |
| 2 | 2 | Reserved, zero |
| 4 | 8 | Call ID, network byte order |
| 12 | 4 | Format generation, network byte order |
| 16 | 4 | Sequence number, network byte order |
| 20 | 4 | Sample timestamp at 48 kHz, network byte order |
| 24 | remaining | One 20 ms Opus frame |

Each sender starts sequence numbers at zero for a generation; timestamps advance by 960 samples per frame. Sequence arithmetic is modulo 2^32. Mono/stereo Opus preserves actual channel order; metadata must match negotiated channels/generation. The server checks stream ownership, call identity, generation, channel count, lengths, and per-user rates before relaying. Invalid, stale, or duplicate media cannot create a call or authorize another destination.

QUIC datagram queues retain at most about ten frames and drop media over 200 ms old. Reliable media queues are bounded as well. Receiver playout begins after a 60 ms startup buffer; late/duplicate frames are discarded, missing frames use Opus concealment, and large forward jumps resynchronize within a bounded window. Control and media remain separate streams. TCP can still delay both under packet loss.

## Security and deployment

The administrator provisions random 256-bit tokens; only SHA-256 hashes are stored, and authentication compares hashes in constant time. User/config files reject symlinks and unsafe ownership/mode. The service limits connections, concurrent request streams, headers, records, control messages, and media frames; unauthenticated connections expire after ten seconds. It does not log tokens or audio. Anonymous UDP packets only reach the QUIC state machine; there is no unauthenticated application media relay.

The server terminates TLS and is trusted with the compressed audio. This is transport encryption, with no end-to-end identity verification. A proxy needs explicit support for these application-specific extended-CONNECT protocols and HTTP Datagrams; ordinary HTTPS GET/POST forwarding alone does not support the media path.
