# Protocol notes

What VeloPlay sends to a PS5 and what comes back, for the steps that are implemented. The
behaviour was learned from the chiaki and chiaki-ng projects, which documented the protocol by
studying the official Remote Play client. File names in brackets point to the matching code in
chiaki-ng.

All numbers are big-endian unless stated otherwise. Only IPv4 is covered.

## Discovery (UDP 9302) [discovery.c]

The client sends one datagram to the broadcast address or straight to a known console:

```
SRCH * HTTP/1.1\n
device-discovery-protocol-version:00030010\n
\0
```

Lines end with a bare line feed and the terminating NUL is part of the datagram. The official
client sends from a local port between 9303 and 9319; VeloPlay tries that range first and falls
back to any free port.

A running console answers:

```
HTTP/1.1 200 Ok
host-id:BC60A7123456
host-type:PS5
host-name:Living Room
host-request-port:997
device-discovery-protocol-version:00030010
system-version:09400004
running-app-name:Astro's Playroom
running-app-titleid:PPSA01325
```

A console in rest mode answers `HTTP/1.1 620 Server Standby` with the same identification lines
and without the `running-app` lines. `host-id` is the MAC address. It identifies a console across
address changes.

Windows sends a datagram for 255.255.255.255 through one network adapter only. VeloPlay therefore
also sends to the directed broadcast address of every active adapter.

## Wake-up (UDP 9302) [discovery.c]

```
WAKEUP * HTTP/1.1\n
client-type:vr\n
auth-type:R\n
model:w\n
app-type:r\n
user-credential:457002842\n
device-discovery-protocol-version:00030010\n
\0
```

`user-credential` is the registration key from pairing. The key is a short hex string such as
`1b3d4f5a`; the packet carries the same number in decimal. The console does not answer. It boots
and then reports status 200 to discovery. That takes 10 to 30 seconds.

The console only listens for this packet if "Stay Connected to the Internet" and "Enable Turning
On PS5 from Network" are enabled in its power saving settings.

## Registration (UDP and TCP 9295) [regist.c, rpcrypt.c]

Pairing happens once per console. The user opens Settings > System > Remote Play > Link Device,
and the console shows an 8-digit PIN for a few minutes.

### 1. Search

The client sends the five bytes `SRC3\0` over UDP to port 9295. A console with the Link Device
screen open answers with a datagram that starts with `RES3`. After about 100 ms it accepts a TCP
connection on the same port.

### 2. Request

One HTTP-like request over TCP:

```
POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n
 HTTP/1.1\r\n
HOST: 10.0.2.15\r\n
User-Agent: remoteplay Windows\r\n
Connection: close\r\n
Content-Length: <payload size>\r\n
RP-Version: 1.0\r\n
\r\n
<payload>
```

The second line and the fixed HOST value look like mistakes. They are what consoles accept, so
they stay.

The payload has two parts.

**Padding block, 480 bytes.** Any filler. Two bytes select columns in two 16x32 key tables, and
sixteen bytes carry the client nonce in masked form:

| Offset | Meaning |
| --- | --- |
| `0x000` | upper five bits = mask column |
| `0x0c7` to `0x0ce` | masked nonce, bytes 8 to 15 |
| `0x18d` | lower five bits = key column |
| `0x191` to `0x198` | masked nonce, bytes 0 to 7 |

Masking, for each byte `i` of the 16-byte random nonce:

```
masked[i] = (nonce[i] XOR table1[i * 32 + maskColumn]) + 0xd3 + i     (mod 256)
```

VeloPlay fills the block with the letter `A`, like chiaki-ng does. That makes key column 1 and
mask column 8.

**Encrypted inner header.** Follows the padding block directly:

```
Client-Type: dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f\r\n
Np-AccountId: <base64 of the 8-byte PSN account ID>\r\n
```

### 3. Encryption

AES-128 in CFB mode with 128-bit feedback.

```
key[i]      = table0[i * 32 + keyColumn]                  for i = 0..15
key[12..15] = key[12..15] XOR PIN as 32-bit big-endian number
iv          = first 16 bytes of HMAC-SHA256(hmacKey, nonce || counter as 64-bit big-endian)
```

The counter is 0 for the request and for the answer. The tables and the HMAC key are protocol
constants (`src/protocol/RegistKeys.hpp`).

The PIN only enters through the key. A wrong PIN makes the inner header decrypt to noise on the
console, which then rejects the request.

### 4. Answer

On success the console sends `HTTP/1.1 200 OK` with a `Content-Length` and a body encrypted with
the same key and IV. Decrypted, the body is a list of header lines:

```
AP-Ssid: ...
AP-Bssid: ...
AP-Key: ...
AP-Name: ...
PS5-Mac: bc60a7123456
PS5-RegistKey: 3162336434663561
PS5-Nickname: Living Room
RP-KeyType: 2
RP-Key: 000102030405060708090a0b0c0d0e0f
```

`PS5-RegistKey` is hex-encoded ASCII (here `1b3d4f5a`), possibly padded with NUL bytes. It
identifies this client towards the console and is the wake-up credential. `RP-Key` is the 16-byte
secret for session authentication. Both are stored encrypted (DPAPI).

On failure the console sends a non-200 status and an error code:

| `RP-Application-Reason` | Meaning |
| --- | --- |
| `80108b09` | registration failed, usually a wrong or expired PIN |
| `80108b02` | account ID does not belong to an account on the console |
| `80108b10` | Remote Play is in use |
| `80108b15` | Remote Play crashed on the console |
| `80108b11` | protocol version mismatch |

## Session and stream (TCP 9295, UDP 9296 and 9297) [session.c, ctrl.c, senkusha.c, takion.c]

This part is not reimplemented. VeloPlay builds chiaki-ng's `libchiaki` and drives it through
`third_party/chiaki/vp_session.h`. The steps, as the library performs them:

1. **Session request**, TCP 9295: `GET /sie/ps5/rp/sess/init` with `RP-Registkey` (the regist key
   as hex of its ASCII form) and `Rp-Version: 1.0`. The console answers with `RP-Nonce`, or with
   an `RP-Application-Reason` from the table above.
2. **Control connection**, a second TCP connection to 9295: authenticated with keys derived from the nonce and `RP-Key`.
   Carries the session id, the login passcode request, the rest mode command and keep-alives.
3. **Senkusha**, UDP 9297: a short test connection that measures round trip time and MTU.
4. **Stream connection**, UDP 9296 (Takion): ECDH key exchange, then the negotiated video
   profile (resolution, frame rate, bitrate, codec), encrypted audio and video with forward error
   correction, and the feedback channel with controller state going the other way.

What VeloPlay asks for: the resolution, frame rate, bitrate and codec from the settings,
automatic downgrade if the console refuses the profile, a new key frame after an unrecoverable
frame, and DualSense features only if a DualSense is the active controller. The library reports
at most 5 % packet loss to the console, as chiaki-ng does by default, so that a short burst does
not push the bitrate to the floor.

The connection over the internet (PSN holepunching, `remote/holepunch.c`) is not built.

## How this is tested

`tests/reference_vectors.hpp` holds values produced by running chiaki-ng's `rpcrypt.c` and
`regist.c` with fixed inputs: derived keys, masked nonces, the IV, a complete request payload and
an encrypted answer. The tests require our output to match byte for byte.

`tests/MockConsole.hpp` plays the console side of all three exchanges on the loopback interface,
so the network code runs end to end in CI.

For sessions, `tests/test_session.cpp` runs the real library against a loopback socket for the
first step: the request line and headers it sends, a refusal, the "in use" answer and stopping
midway. Everything after the session request needs a console. `tests/FakeSession.hpp` stands in
for the library where the application logic is tested (wake first, retry, reconnect, passcode,
rest mode).

None of this replaces a test against a real console.
