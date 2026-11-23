# libchiaki inside VeloPlay

VeloPlay uses libchiaki from [chiaki-ng](https://github.com/streetpea/chiaki-ng) for everything
between "the console accepted the session request" and "here is a video frame": the control
connection, the Takion transport, forward error correction, stream encryption and the feedback
channel that carries controller input. Discovery, wake-up and pairing are VeloPlay's own code in
`src/protocol`.

## What is built

`CMakeLists.txt` fetches four repositories at fixed commits and builds static libraries from them:

| Target | Source | What for |
| --- | --- | --- |
| `veloplay_chiaki` | chiaki-ng `lib/src` | the protocol |
| `veloplay_jerasure`, `veloplay_gf_complete` | streetpea's forks, as chiaki-ng pins them | forward error correction |
| `veloplay_nanopb` | nanopb 0.4.9.2 | the protobuf messages of Takion |

Nothing of chiaki-ng's own build system is used.

## The connection through Sony's servers

The option `VELOPLAY_PSN` (on for Windows, off elsewhere) builds `aia.c`, `aia_openssl.c` and
`remote/holepunch.c` with curl (with WebSockets), json-c, libevent and miniupnpc from vcpkg. The
Linux CI keeps it off because the system curl has no WebSockets; a local Linux build with the
option on compiles and links, which is the check for the C code there.

- `chiaki_holepunch_upnp_discover` is never called, so the gateway status stays unknown and
  holepunch.c neither adds nor deletes port mappings on the router. Its address comes from STUN.
- `holepunch.c` is built from a patched copy: two calls pass a server message to the logger as the
  format string. They get `"%s"` instead. The build stops if upstream changes those lines.
- `vp_session.c` drops log lines that carry server messages, hex dumps or tokens, and masks IPv4
  addresses.

## What is left out or replaced

- Without `VELOPLAY_PSN`: `aia.c`, `aia_openssl.c`, `remote/holepunch.c` are not compiled;
  `common.c` is compiled from a copy with the two lines removed that call into `aia.c` (the build
  stops if upstream changes those lines); `holepunch_stub.c` provides the holepunch functions the
  session code references, which are only reached with a holepunch session.
- `chiaki/config.h`: generated, with the built-in Opus, FFmpeg and Raspberry Pi decoders off.
  VeloPlay decodes itself.
- `generated/takion.pb.c|h`: output of nanopb's generator for `lib/protobuf/takion.proto`, checked
  in so that a build needs neither protoc nor Python.

## The interface

`vp_session.h` and `vp_session.c` are VeloPlay code. They wrap a session in plain C types. The rest
of the client includes only this header, through `src/session/ChiakiSession.cpp`.

The reason is the compiler: libchiaki is C with variable-length arrays, which Microsoft's compiler
does not accept. On Windows these targets are therefore built with the ClangCL toolset of Visual
Studio (component "C++ Clang tools for Windows"), and the client with MSVC. Both follow the same
ABI, and the narrow interface keeps the amount of shared declarations small.

## Updating

1. Pick the new chiaki-ng commit and the submodule commits it pins (`git submodule status` in a
   checkout) and enter them in `CMakeLists.txt`.
2. If `lib/protobuf/takion.proto` changed, regenerate the two files in `generated/` the way
   chiaki-ng's `lib/protobuf/CMakeLists.txt` does:
   `protoc -otakion.pb takion.proto -I<chiaki-ng>/lib/protobuf`, then
   `python3 <nanopb>/generator/nanopb_generator.py takion.pb`.
3. Check the source list against chiaki-ng's `lib/CMakeLists.txt`.
4. `ctest --preset core`. The `ChiakiSession.*` tests run the library against a loopback socket.
5. Test against a real console. Nothing else shows whether a firmware update changed the protocol.
