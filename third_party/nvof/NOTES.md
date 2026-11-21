# Using NVOFA from Direct3D 11: notes from SDK 5.0.7

Everything here was read from the headers in this folder, from the SDK sample code
(`Common/NvOFBase/NvOF.cpp`, `NvOFD3D11.cpp`, `NvOFD3DCommon.cpp`,
`NvOFBasicSamples/AppOFD3D11/AppOFD3D11.cpp`) and from the PDFs that come with the SDK
(`Read_Me.pdf`, `Doc/NVOFA_Programming_Guide.pdf`, `Doc/NVOFA_Application_Note.pdf`). Code
blocks are copied from the headers. Text from the PDFs is summarised, not copied; the PDFs
themselves are not in the repository. Nothing here comes from memory.

## Library and entry point

- DLL: `nvofapi64.dll` on 64-bit Windows (`nvofapi.dll` on 32-bit). It is part of the display
  driver; the sample loads it with `LoadLibrary(TEXT("nvofapi64.dll"))` and fails with
  "NVOF library file not found. Please ensure that the NVIDIA driver is installed".
- Exported function, looked up with `GetProcAddress(module, "NvOFAPICreateInstanceD3D11")`:

```c
NV_OF_STATUS NVOFAPI NvOFAPICreateInstanceD3D11(uint32_t apiVer, NV_OF_D3D11_API_FUNCTION_LIST  *functionList);
```

- `NVOFAPI` is `__stdcall` on Windows.
- Version:

```c
#define NV_OF_API_MAJOR_VERSION 5
#define NV_OF_API_MINOR_VERSION 0
#define NV_OF_API_VERSION  (uint16_t)((NV_OF_API_MAJOR_VERSION << 4) | NV_OF_API_MINOR_VERSION)
```

  That is `0x50`. To ask the driver for the highest version it supports (guide, section 7.3),
  `nvOpticalFlowCommon.h` declares
  `NV_OF_STATUS NVOFAPI NvOFGetMaxSupportedApiVersion(uint32_t* version);`, also exported by
  the DLL.

## Function list for Direct3D 11 (order as in the header)

```c
typedef struct _NV_OF_D3D11_API_FUNCTION_LIST
{
    PFNNVCREATEOPTICALFLOWD3D11                                nvCreateOpticalFlowD3D11;
    PFNNVOFINIT                                                nvOFInit;
    PFNNVOFGETSURFACEFORMATCOUNTD3D11                          nvOFGetSurfaceFormatCountD3D11;
    PFNNVOFGETSURFACEFORMATD3D11                               nvOFGetSurfaceFormatD3D11;
    PFNNVOFREGISTERRESOURCED3D11                               nvOFRegisterResourceD3D11;
    PFNNVOFUNREGISTERRESOURCED3D11                             nvOFUnregisterResourceD3D11;
    PFNNVOFEXECUTE                                             nvOFExecute;
    PFNNVOFDESTROY                                             nvOFDestroy;
    PFNNVOFGETLASTERROR                                        nvOFGetLastError;
    PFNNVOFGETCAPS                                             nvOFGetCaps;
} NV_OF_D3D11_API_FUNCTION_LIST;
```

Signatures:

```c
typedef NV_OF_STATUS(NVOFAPI* PFNNVCREATEOPTICALFLOWD3D11) (ID3D11Device* const pD3D11Device, ID3D11DeviceContext* const pD3D11DeviceContext, NvOFHandle *hOFInstance);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFINIT) (NvOFHandle hOf, const NV_OF_INIT_PARAMS *initParams);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFGETSURFACEFORMATCOUNTD3D11) (NvOFHandle hOf, const NV_OF_BUFFER_USAGE bufUsage, const NV_OF_MODE ofMode, uint32_t* const pCount);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFGETSURFACEFORMATD3D11) (NvOFHandle hOf, const NV_OF_BUFFER_USAGE bufUsage, const NV_OF_MODE ofMode, DXGI_FORMAT* const pFormat);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFREGISTERRESOURCED3D11) (NvOFHandle hOf, ID3D11Resource* pResource, NvOFGPUBufferHandle* const hOFGpuBuffer);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFUNREGISTERRESOURCED3D11) (NvOFGPUBufferHandle hOFGpuBuffer);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFEXECUTE) (NvOFHandle hOf, const NV_OF_EXECUTE_INPUT_PARAMS *executeInParams, NV_OF_EXECUTE_OUTPUT_PARAMS *executeOutParams);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFDESTROY) (NvOFHandle hOf);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFGETLASTERROR) (NvOFHandle hOf, char lastError[], uint32_t *size);
typedef NV_OF_STATUS(NVOFAPI* PFNNVOFGETCAPS) (NvOFHandle hOf, NV_OF_CAPS capsParam, uint32_t *capsVal, uint32_t *size);
```

Handles are opaque pointers: `NvOFHandle`, `NvOFGPUBufferHandle`, `NvOFPrivDataHandle`.

## NV_OF_INIT_PARAMS

```c
typedef struct _NV_OF_INIT_PARAMS
{
    uint32_t                        width;
    uint32_t                        height;
    NV_OF_OUTPUT_VECTOR_GRID_SIZE   outGridSize;
    NV_OF_HINT_VECTOR_GRID_SIZE     hintGridSize;
    NV_OF_MODE                      mode;
    NV_OF_PERF_LEVEL                perfLevel;
    NV_OF_BOOL                      enableExternalHints;
    NV_OF_BOOL                      enableOutputCost;
    NvOFPrivDataHandle              hPrivData;
    NV_OF_STEREO_DISPARITY_RANGE    disparityRange;
    NV_OF_BOOL                      enableRoi;
    NV_OF_PRED_DIRECTION            predDirection;
    NV_OF_BOOL                      enableGlobalFlow;
    NV_OF_BUFFER_FORMAT             inputBufferFormat;
} NV_OF_INIT_PARAMS;
```

(Field comments left out; see the header. `hPrivData` is reserved and must be `NULL`.
`hintGridSize` counts only with `enableExternalHints` and must be at least `outGridSize`.
`disparityRange` is for stereo mode; `UNDEFINED` on Turing.)

Enums used there:

```c
NV_OF_MODE:           NV_OF_MODE_UNDEFINED, NV_OF_MODE_OPTICALFLOW, NV_OF_MODE_STEREODISPARITY, NV_OF_MODE_MAX
NV_OF_PERF_LEVEL:     NV_OF_PERF_LEVEL_UNDEFINED, NV_OF_PERF_LEVEL_SLOW = 5, NV_OF_PERF_LEVEL_MEDIUM = 10, NV_OF_PERF_LEVEL_FAST = 20, NV_OF_PERF_LEVEL_MAX
NV_OF_OUTPUT_VECTOR_GRID_SIZE: _UNDEFINED, _1 = 1, _2 = 2, _4 = 4, _MAX
NV_OF_HINT_VECTOR_GRID_SIZE:   _UNDEFINED, _1 = 1, _2 = 2, _4 = 4, _8 = 8, _MAX
NV_OF_PRED_DIRECTION: NV_OF_PRED_DIRECTION_FORWARD = 0, NV_OF_PRED_DIRECTION_BOTH = 2, NV_OF_PRED_DIRECTION_MAX
NV_OF_BOOL:           NV_OF_FALSE = 0, NV_OF_TRUE = !NV_OF_FALSE
```

What the SDK sample code sets (`NvOF::Init` after `memset(&m_initParams, 0, ...)`):

| Field | Value in the sample |
| --- | --- |
| `width`, `height` | size of the input frames |
| `inputBufferFormat` | format of the input buffers (the YUV loader gives `NV_OF_BUFFER_FORMAT_NV12`, the image loader `NV_OF_BUFFER_FORMAT_ABGR8`) |
| `mode` | `NV_OF_MODE_OPTICALFLOW` (`AppOFD3D11`) |
| `perfLevel` | command line `--preset`, default `"medium"` = `NV_OF_PERF_LEVEL_MEDIUM` |
| `outGridSize` | command line `--gridSize`, default 1. If the card does not support it (`NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES`), the next larger supported size is used and the result is upsampled |
| `hintGridSize` | `NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED` (default argument) |
| `enableExternalHints` | `NV_OF_FALSE` (default argument) |
| `enableOutputCost` | `NV_OF_FALSE` |
| `enableRoi` | false unless an ROI file is given |
| `predDirection`, `enableGlobalFlow`, `disparityRange`, `hPrivData` | left at 0 by the `memset` (0 = `NV_OF_PRED_DIRECTION_FORWARD`) |

The guide recommends the 8-bit cost buffer (`NV_OF_BUFFER_FORMAT_UINT8`) over the 32-bit one if
cost is enabled; the 32-bit one is to be deprecated.

## Formats

`NV_OF_BUFFER_FORMAT` and the DXGI format the sample maps it to (`NvOFD3DCommon.cpp`,
`DXGIFormatToNvOFBufferFormat` / `NvOFBufferFormatToDxgiFormat`):

| NVOF format | Use | DXGI format |
| --- | --- | --- |
| `NV_OF_BUFFER_FORMAT_GRAYSCALE8` | input, 8-bit planar | `DXGI_FORMAT_R8_UNORM` |
| `NV_OF_BUFFER_FORMAT_NV12` | input, 8-bit planar, UV interleaved | `DXGI_FORMAT_NV12` |
| `NV_OF_BUFFER_FORMAT_ABGR8` | input, 8-bit packed A8B8G8R8 | `DXGI_FORMAT_B8G8R8A8_UNORM` |
| `NV_OF_BUFFER_FORMAT_SHORT2` | output or hint, optical flow vectors | `DXGI_FORMAT_R16G16_SINT` |
| `NV_OF_BUFFER_FORMAT_SHORT` | output or hint, stereo disparity | `DXGI_FORMAT_R16_UINT` |
| `NV_OF_BUFFER_FORMAT_UINT` | legacy 32-bit cost | `DXGI_FORMAT_R32_UINT` |
| `NV_OF_BUFFER_FORMAT_UINT8` | 8-bit cost | not in the sample's D3D mapping |

Which formats the driver really accepts is asked at run time with
`nvOFGetSurfaceFormatCountD3D11` / `nvOFGetSurfaceFormatD3D11` per `NV_OF_BUFFER_USAGE` and
mode; the sample refuses to start unless its input format and `SHORT2` (output) are in the
list. The decoder in VeloPlay produces NV12 (D3D11VA), which is one of the input formats.
The guide says input frames must not carry padding.

Output vector:

```c
typedef struct _NV_OF_FLOW_VECTOR
{
    int16_t                         flowx;        /**< x component of flow in S10.5 format */
    int16_t                         flowy;        /**< y component of flow in S10.5 format */
} NV_OF_FLOW_VECTOR;
```

- S10.5 fixed point: per the guide the lowest 5 bits are the fraction, then 10 integer bits,
  then the sign bit. So the value divided by 32 is the motion in pixels.
- Direction (header, `NV_OF_PRED_DIRECTION_FORWARD`): each vector is the position change of a
  pixel from `inputFrame` to `referenceFrame`. With `NV_OF_PRED_DIRECTION_BOTH` the backward
  field (`referenceFrame` to `inputFrame`) goes to `bwdOutputBuffer`.
- Output size: one vector per grid cell, `(width + grid - 1) / grid` by
  `(height + grid - 1) / grid` (`NvOF::Init`). Texture format `DXGI_FORMAT_R16G16_SINT`.
- In `AppOFD3D11` the first frame of a pair is `inputBuffers[i]` (passed as `inputFrame`) and the
  second `inputBuffers[i + 1]` (`referenceFrame`), so the vectors point from the earlier frame
  to the later one.

## Execute parameters

```c
typedef struct _NV_OF_EXECUTE_INPUT_PARAMS
{
    NvOFGPUBufferHandle             inputFrame;
    NvOFGPUBufferHandle             referenceFrame;
    NvOFGPUBufferHandle             externalHints;
    NV_OF_BOOL                      disableTemporalHints;
    uint32_t                        padding;
    NvOFPrivDataHandle              hPrivData;
    uint32_t                        padding2;
    uint32_t                        numRois;
    NV_OF_ROI_RECT*                 roiData;
} NV_OF_EXECUTE_INPUT_PARAMS;

typedef struct _NV_OF_EXECUTE_OUTPUT_PARAMS
{
    NvOFGPUBufferHandle            outputBuffer;
    NvOFGPUBufferHandle            outputCostBuffer;
    NvOFPrivDataHandle             hPrivData;
    NvOFGPUBufferHandle            bwdOutputBuffer;
    NvOFGPUBufferHandle            bwdOutputCostBuffer;
    NvOFGPUBufferHandle            globalFlowBuffer;
} NV_OF_EXECUTE_OUTPUT_PARAMS;
```

- `padding`, `padding2` must be 0, `hPrivData` `NULL`.
- `disableTemporalHints`: with 0 the vectors of the previous `Execute` serve as hints for the
  next one, which the header says helps on a continuous video without big scene changes; for
  unrelated image pairs it recommends 1. The sample passes the default `NV_OF_FALSE`.
- The sample fills both structs after a `memset` to 0 and sets only `inputFrame`,
  `referenceFrame`, `disableTemporalHints`, `externalHints` (only with hints on), `numRois`,
  `roiData`, `outputBuffer` and `outputCostBuffer` (only with cost on).

## Sequence in the Direct3D 11 sample

1. `LoadLibrary("nvofapi64.dll")`, `GetProcAddress("NvOFAPICreateInstanceD3D11")`.
2. `D3D11CreateDevice` on the chosen adapter (`EnumAdapters(gpuId)`, `D3D_DRIVER_TYPE_UNKNOWN`).
3. `NvOFAPICreateInstanceD3D11(NV_OF_API_VERSION, &list)` fills the function list.
4. `nvCreateOpticalFlowD3D11(device, context, &hOf)`.
5. `nvOFGetSurfaceFormatCountD3D11` + `nvOFGetSurfaceFormatD3D11` for `NV_OF_BUFFER_USAGE_INPUT`
   and `NV_OF_BUFFER_USAGE_OUTPUT`; check that the wanted formats are there.
6. `nvOFGetCaps(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, ...)` (first with `capsVal = nullptr`
   to get the count, then with an array), optionally `NV_OF_CAPS_SUPPORT_ROI`.
7. Fill `NV_OF_INIT_PARAMS` (table above) and call `nvOFInit`.
8. Buffers: for each, `CreateTexture2D` with `Width`/`Height` of the buffer, `MipLevels = 1`,
   `ArraySize = 1`, the DXGI format from the table, `SampleDesc.Count = 1`,
   `Usage = D3D11_USAGE_DEFAULT`,
   `BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS`,
   `CPUAccessFlags = 0`; then `nvOFRegisterResourceD3D11(hOf, texture, &handle)`. The sample
   makes 16 input buffers and 15 output buffers and a staging copy of each for upload and
   download. An existing texture can be registered the same way (`DoRegisterBuffers`).
9. Upload a frame: `Map` the staging texture (`D3D11_MAP_WRITE`), copy rows (NV12: height
   times 1.5), `Unmap`, `CopyResource(texture, staging)`.
10. `nvOFExecute(hOf, &in, &out)` for every pair of frames.
11. Read back (only in the sample): `CopyResource(staging, texture)`, `Map` with
    `D3D11_MAP_READ`. Its `SyncBuffer` does just that to wait for the GPU before timing.
12. Teardown (guide, section 6.1): `nvOFUnregisterResourceD3D11` for every registered resource,
    release the textures, `nvOFDestroy(hOf)`, then the context and the device.

Further points from the guide's usage guidelines (summarised):

- For Direct3D 11 the process needs no explicit fences; Direct3D 12 and Vulkan do.
- The API is not thread safe; a session used from several threads needs a lock.
- Keep a pool of input buffers and use them round robin instead of reusing one pair;
  4 to 6 input buffers are said to be enough to keep the engine busy.
- Use the SLOW preset only when there is spare graphics bandwidth; it costs more GPU time.
- External hints get the highest weight in the search, so only good vectors should be given.

## Hardware and driver

- Turing and later, except TU117 (application note). Turing can do only the 4x4 grid; 2x2 and
  1x1 need Ampere or later. Hardware cost and ROI also need Ampere or later. Maximum input:
  4096x4096 on Turing, 8192x8192 on Ampere and Ada.
- Driver: Windows 528.24 or later (`Read_Me.pdf`).
- Windows: the guide says DirectX 11 needs Windows 8 or later, DirectX 12 Windows 10 20H1 or
  later; `Read_Me.pdf` says Windows 10 or later for the CUDA, DirectX 11 and Vulkan interfaces.
- The application note has a table of indicative speed per preset and grid at 1080p; its PDF
  layout does not survive text extraction, so no numbers are copied here.

## The user's PC (21.11.2026)

- GPU: NVIDIA GeForce RTX 4070 (Ada), driver 617.14.
- `C:\Windows\System32\nvofapi64.dll` is present (file version 32.0.16.1714).
- Windows 11, build 26300.
