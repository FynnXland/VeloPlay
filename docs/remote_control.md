# Remote control for scripts

VeloPlay can take commands from other programs on the same PC: press buttons, move sticks, read
the status. The physical controller keeps working at the same time and wins whenever both give
input.

The interface is off by default. Turn it on under Settings > Advanced > Remote control for
scripts.

## Transport

- UDP, one JSON object per datagram, at most 4096 bytes.
- VeloPlay listens on `127.0.0.1` only, port 9876 unless changed. Other devices on the network
  cannot reach it.
- Every request gets one JSON object back, sent to the address the request came from.
- If a password is set in the settings, every request needs `"token": "<password>"`.
- An `"id"` in the request is copied into the answer, so a script can match the two.

UDP was chosen on purpose. A web page can open a WebSocket to localhost, but it cannot send UDP,
so a page in your browser cannot press buttons on your console.

## Answers

```json
{"ok": true}
{"ok": false, "error": "invalid_argument", "message": "\"duration_ms\" is out of range"}
```

Error codes: `invalid_json`, `invalid_argument`, `unknown_command`, `unauthorized`,
`no_active_session`, `not_available`.

## Commands

### ping

```json
{"cmd": "ping"}
```

Returns `app`, `version` and `protocol` (currently 1).

### status

```json
{"cmd": "status"}
```

```json
{"ok": true, "status": {
  "state": "idle",
  "profile": "smooth30",
  "controller": "DualSense Wireless Controller",
  "consoles": [{"name": "Living Room", "address": "192.168.1.20", "status": "ready", "paired": true}]
}}
```

`state` is one of `idle`, `waking`, `connecting`, `streaming`, `reconnecting`, `disconnecting`,
`error`. A console's `status` is `ready`, `standby` or `offline`.

### press

Presses buttons for a fixed time.

```json
{"cmd": "press", "buttons": ["cross"], "duration_ms": 120}
```

`duration_ms` is optional (default 100, 1 to 10000). Several presses may overlap.

Button names: `cross`, `circle`, `square`, `triangle`, `dpad_up`, `dpad_down`, `dpad_left`,
`dpad_right`, `l1`, `r1`, `l3`, `r3`, `options`, `create`, `touchpad`, `ps`, `mute`.

### set_state

Sets held buttons, sticks and triggers in one go. The state expires on its own, so a script that
crashes cannot leave a stick pushed forward. To hold something, keep sending.

```json
{"cmd": "set_state", "buttons": ["l1"], "left_stick": [0.0, -1.0], "r2": 0.6, "ttl_ms": 250}
```

- `left_stick`, `right_stick`: `[x, y]`, each from -1 to 1. Right and down are positive.
- `l2`, `r2`: 0 to 1.
- `ttl_ms`: lifetime in milliseconds (default 250, 1 to 5000).

Fields that are left out are neutral. Each call replaces the previous state.

### release_all

```json
{"cmd": "release_all"}
```

Drops everything set by `press` and `set_state`.

### action

```json
{"cmd": "action", "name": "next_profile"}
```

| Name | Effect | Available |
| --- | --- | --- |
| `ps_button` | short press of the PS button | always |
| `ps_button_hold` | PS button held for 1.5 s | always |
| `next_profile` | switches to the next picture mode | always |
| `sleep` | puts the console into rest mode and ends the session | while connected |
| `disconnect` | ends the session | while connected |
| `screenshot` | saves the current picture | not yet |
| `toggle_microphone` | mutes or unmutes the microphone | not yet |
| `toggle_frame_generation` | frame generation on or off | not yet |

`sleep` and `disconnect` answer `no_active_session` when no console is connected. The last three
arrive together with picture and sound; until then they answer `not_available` while connected.

While a console is connected, everything sent with `press` and `set_state` reaches it together
with the input of the physical controller, and `status` carries a `session` object:

```json
{"ok": true, "status": {"state": "streaming", "session": {"console": "PS5-042", "connected": true}}}
```

Without a session, `session` is `null`.

## How scripted and physical input combine

- Buttons add up. A script can press a button but cannot release one you are holding.
- A stick or trigger follows the physical controller as soon as you move it. It follows the
  script only while the physical one is at rest.

## Example (Python)

```python
import json
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(1.0)

def send(command):
    sock.sendto(json.dumps(command).encode(), ("127.0.0.1", 9876))
    return json.loads(sock.recvfrom(4096)[0])

print(send({"cmd": "status"}))
print(send({"cmd": "press", "buttons": ["cross"], "duration_ms": 150}))
```
