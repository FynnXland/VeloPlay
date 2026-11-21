# NVIDIA Optical Flow interface headers

The three headers here are the interface of the optical flow engine in NVIDIA graphics cards
(NVOFA). They are copied unchanged from the NVIDIA Optical Flow SDK; `.gitattributes` keeps
their CRLF line endings so the files stay byte for byte as shipped.

| File | SHA-256 |
| --- | --- |
| `nvOpticalFlowCommon.h` | `fb96fc6061c01ec2c5970ecebebacac7535c033717ff39810cada1f197c8ec9f` |
| `nvOpticalFlowD3D11.h` | `fdb83ba0ef18c7ddd03e8cdb3eb14660795415c84d9f0f40d83881bbdd60900e` |
| `nvOpticalFlowD3D12.h` | `d1952d0946a26f1b77e76c320b4dfe9077d946fdc1a6195e01f0460362026305` |

- SDK: Optical Flow SDK 5.0.7 (`Optical_Flow_SDK_5.0.7.zip`, folder `NvOFInterface`),
  `NV_OF_API_VERSION` 5.0
- Source: https://developer.nvidia.com/opticalflow-sdk (download needs an NVIDIA developer
  account). The public repository `NVIDIA/NVIDIAOpticalFlowSDK` carries only the CUDA parts.
- Downloaded: 21.11.2026
- Not copied: the CUDA and Vulkan headers, the sample code, the binaries and the documentation.

Nothing here is linked. The driver installs `nvofapi64.dll`; VeloPlay would load it at run time
and look up `NvOFAPICreateInstanceD3D11`. `NOTES.md` collects what the headers, the samples
and the programming guide say about using the engine from Direct3D 11.

## License

The SDK as a whole is under the "NVIDIA DesignWorks SDK License" (`LicenseAgreement.pdf` in the
download, v. May 10, 2022), which does not allow redistribution of the SDK. Its section 7
("Components under other licenses") says that components with their own notices are governed
by those notices where the two conflict. Each of these headers carries its own notice, which
says it applies to that header file only and is the MIT license:

```
This copyright notice applies to this header file only:

Copyright (c) 2018-2023 NVIDIA Corporation

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the software, and to permit persons to whom the
software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

The years differ per file: 2018-2023 for `nvOpticalFlowCommon.h`, 2019-2023 for
`nvOpticalFlowD3D11.h`, 2020-2023 for `nvOpticalFlowD3D12.h`. The rest of the notice is the same.

The sample code of the SDK (`Common/NvOFBase`, `NvOFBasicSamples`) also carries this MIT notice
but is not copied. The frame interpolation library that comes with the SDK (`NvOFFRUC.h`,
`NvOFFRUC.dll`) is under NVIDIA's EULA ("Any use, reproduction, disclosure, or distribution ...
outside the terms of the EULA is strictly prohibited") and must not be used or shipped by
VeloPlay.
