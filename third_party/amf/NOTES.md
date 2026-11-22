# AMD Advanced Media Framework (AMF): headers for frame rate conversion

The headers under `include/` are copied unchanged from
https://github.com/GPUOpen-LibrariesAndSDKs/AMF (folder `amf/public/include`, commit
`57ee0cfb0e1a08d9041f77435653794c1b012be0`), only what `components/FRC.h` needs. They are under the MIT licence; each file carries
its own notice (Copyright (c) Advanced Micro Devices, Inc.).

Nothing is linked: `src/render/AmfFrc.cpp` loads `amfrt64.dll`, which AMD's graphics driver
installs into the system folder, and asks it for `AMFInit`. On a PC without an AMD card the DLL is
missing and VeloPlay keeps its own method.

What VeloPlay uses (from `amf/doc/AMF_FRC_API.md` and `components/FRC.h`):

- Context on our Direct3D 11 device (`InitDX11`), component `AMFFRC`.
- `AMF_FRC_ENGINE_TYPE` = `FRC_ENGINE_DX11`, `AMF_FRC_MODE` = `FRC_ONLY_INTERPOLATED` (only the
  picture in between comes back), `AMF_FRC_USE_FUTURE_FRAME` = false (the header's default is true
  and costs a whole picture of delay), `AMF_FRC_INDICATOR` = false (the header says false, the guide
  says true; set either way), `AMF_FRC_ENABLE_FALLBACK` = true (blend when unsure), profile
  `FRC_PROFILE_HIGH` (up to 1440p).
- Input and output formats must be the same; VeloPlay hands in RGBA 8 bit (`AMF_SURFACE_RGBA`).
- Always 2x: one picture in between, at the middle. For 3x and 4x VeloPlay's own method runs.
