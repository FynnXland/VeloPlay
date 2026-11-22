# Third-party code in VeloPlay

VeloPlay itself is licensed under the GNU Affero General Public License, version 3 (see `LICENSE`).
The Windows package also contains the libraries listed here. Their sources are fetched at fixed
commits when VeloPlay is built; `third_party/chiaki/CMakeLists.txt` names the commits.

## libchiaki (chiaki-ng)

Session setup, Takion transport, error correction and stream encryption of the Remote Play protocol.

- Source: https://github.com/streetpea/chiaki-ng, directory `lib`
- License: GNU Affero General Public License, version 3 only, with an additional permission to
  link with OpenSSL (`LICENSES/AGPL-3.0-only-OpenSSL.txt` in that repository)
- Copyright: the Chiaki and chiaki-ng authors
- Built without the parts that connect through the internet. `third_party/chiaki/README.md` lists
  what is left out and what is replaced.

## Account lookup (chiaki-ng, grill2010)

The steps VeloPlay performs to find out the PSN account ID after a sign-in at Sony, the addresses
it talks to and the client identification it uses are taken from `scripts/psn-account-id.py` in
chiaki-ng (AGPL-3.0-only), which credits grill2010 for working the procedure out. The code in
`src/protocol/PsnAccount.cpp` is VeloPlay's own.

## Jerasure

```
Copyright (c) 2013, James S. Plank and Kevin Greenan
All rights reserved.

Jerasure - A C/C++ Library for a Variety of Reed-Solomon and RAID-6 Erasure Coding Techniques

Revision 2.0: Galois Field backend now links to GF-Complete

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

 - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

 - Neither the name of the University of Tennessee nor the names of its
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## GF-Complete

```
Copyright (c) 2013, James S. Plank, Ethan L. Miller, Kevin M. Greenan,
Benjamin A. Arnold, John A. Burnum, Adam W. Disney, Allen C. McBride
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

 - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

 - Neither the name of the University of Tennessee nor the names of its
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## nanopb

```
Copyright (c) 2011 Petteri Aimonen <jpa at nanopb.mail.kapsi.fi>

This software is provided 'as-is', without any express or 
implied warranty. In no event will the authors be held liable 
for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any 
purpose, including commercial applications, and to alter it and 
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you 
   must not claim that you wrote the original software. If you use 
   this software in a product, an acknowledgment in the product 
   documentation would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and 
   must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source 
   distribution.
```

## NVIDIA Optical Flow interface headers

`third_party/nvof/nvOpticalFlowCommon.h`, `nvOpticalFlowD3D11.h` and `nvOpticalFlowD3D12.h`,
unchanged from the NVIDIA Optical Flow SDK 5.0.7 (https://developer.nvidia.com/opticalflow-sdk).

- License: MIT, per the notice at the top of each file, which applies to that header file only
- Copyright (c) 2018-2023 NVIDIA Corporation (the years differ per file, see
  `third_party/nvof/README.md`)
- Nothing else from the SDK is included. The optical flow library itself (`nvofapi64.dll`) is
  part of the NVIDIA display driver and is not shipped with VeloPlay.

## Libraries from vcpkg

WebView2 SDK (BSD 3-Clause; the browser itself is part of Windows), FFmpeg (libavcodec and libavutil, LGPL 2.1 or later; built without GPL or non-free parts, shipped
as separate DLLs that can be replaced), Opus (BSD 3-Clause), OpenSSL (Apache License 2.0), SDL3
(zlib license), Dear ImGui (MIT), nlohmann/json (MIT), spdlog (MIT), fmt (MIT) and miniz (MIT).
FFmpeg's source is available from https://ffmpeg.org; the exact version is the one vcpkg builds
for the `builtin-baseline` named in `vcpkg.json`. vcpkg places each library's license text under
`vcpkg_installed/<triplet>/share/<name>/copyright` in the build directory.

## Sharpening (RCAS)

The sharpening pass in `src/render/VideoRenderer.cpp` follows the method AMD published as
"Robust Contrast Adaptive Sharpening" in FidelityFX Super Resolution 1.0. The shader here was
written from the description of the method, not copied. FidelityFX Super Resolution is
Copyright (c) 2021 Advanced Micro Devices, Inc. and available under the MIT licence:
https://github.com/GPUOpen-Effects/FidelityFX-FSR

## AMD Advanced Media Framework (AMF)

The headers in `third_party/amf/include` come unchanged from AMD's AMF SDK
(https://github.com/GPUOpen-LibrariesAndSDKs/AMF), MIT licence, Copyright (c) Advanced Micro Devices,
Inc. They describe AMF's frame rate conversion (FRC). The runtime itself, `amfrt64.dll`, is part of
AMD's graphics driver and is loaded only on PCs with an AMD card; it is not shipped with VeloPlay.

## Debanding

The deband pass in `src/render/VideoRenderer.cpp` follows a widely used idea: compare each pixel
with a few neighbours at a random distance and replace it with their average only where they all
lie within a small threshold, then dither. It is known from the flash3kyuu_deband filter for
AviSynth and from libplacebo's deband shader. No code was taken from either; the shader, its
thresholds and its noise were written for VeloPlay.

## DualSense trigger effects

The byte layouts of the three effects VeloPlay uses to try the adaptive triggers (resistance,
pressure point, shaking; `src/input/DualSenseEffects.cpp`) were worked out and published by
John "Nielk1" Klein as "Factories for all DualSense trigger effects", under the MIT licence,
Copyright (c) 2021-2022 John "Nielk1" Klein:
https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db
The code here was written from that description, not copied.
