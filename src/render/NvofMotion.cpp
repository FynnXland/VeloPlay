#include "render/NvofMotion.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <dxgi.h>
#include <windows.h>

#include "nvOpticalFlowD3D11.h"

// What is known about the engine comes from NVIDIA's headers, the samples and
// the guide of the Optical Flow SDK 5.0.7; third_party/nvof/NOTES.md has it
// in one place. The order of calls here is the one of the SDK's Direct3D 11 sample.

namespace veloplay::render {

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kVendorNvidia = 0x10DE;
// The sample maps the packed 8 bit input format of the engine to this.
constexpr DXGI_FORMAT kInputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16_SINT;

using CreateInstance = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);

} // namespace

struct NvofMotion::Engine {
    HMODULE library = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    NvOFHandle session = nullptr;
    NvOFGPUBufferHandle inputs[2] = {nullptr, nullptr};
    NvOFGPUBufferHandle output = nullptr;
};

NvofMotion::NvofMotion() = default;

NvofMotion::~NvofMotion() {
    stop();
    if (engine_) {
        if (engine_->library) FreeLibrary(engine_->library);
        delete engine_;
    }
}

std::string NvofMotion::lastError(const char* what, int status) {
    std::string text = std::string(what) + " failed with status " + std::to_string(status);
    if (engine_ && engine_->session && engine_->api.nvOFGetLastError) {
        char message[512] = {};
        uint32_t size = sizeof(message);
        if (engine_->api.nvOFGetLastError(engine_->session, message, &size) == NV_OF_SUCCESS && message[0] != 0) {
            message[sizeof(message) - 1] = 0;
            text += ": ";
            text += message;
        }
    }
    return text;
}

bool NvofMotion::create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error) {
    device_ = device;
    context_ = context;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetDesc(&description))) {
        error = "the graphics adapter could not be identified";
        return false;
    }
    if (description.VendorId != kVendorNvidia) {
        error = "not an NVIDIA graphics card";
        return false;
    }

    // The library belongs to the driver and lies in the system folder; nowhere else is looked.
    HMODULE library = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) {
        error = "the driver's optical flow library (nvofapi64.dll) was not found";
        return false;
    }
    const auto createInstance =
        reinterpret_cast<CreateInstance>(reinterpret_cast<void*>(GetProcAddress(library, "NvOFAPICreateInstanceD3D11")));
    engine_ = new Engine;
    engine_->library = library;
    if (!createInstance) {
        error = "nvofapi64.dll has no entry for Direct3D 11";
        return false;
    }
    const NV_OF_STATUS listed = createInstance(NV_OF_API_VERSION, &engine_->api);
    if (listed != NV_OF_SUCCESS || !engine_->api.nvCreateOpticalFlowD3D11 || !engine_->api.nvOFInit ||
        !engine_->api.nvOFRegisterResourceD3D11 || !engine_->api.nvOFExecute || !engine_->api.nvOFDestroy) {
        error = "the driver does not offer the optical flow interface in the version asked for (status " +
                std::to_string(static_cast<int>(listed)) + ")";
        return false;
    }
    // Whether this card has the engine shows when a session is opened.
    const NV_OF_STATUS opened = engine_->api.nvCreateOpticalFlowD3D11(device_.Get(), context_.Get(), &engine_->session);
    if (opened != NV_OF_SUCCESS || !engine_->session) {
        engine_->session = nullptr;
        error = "this card or driver has no optical flow engine (status " + std::to_string(static_cast<int>(opened)) + ")";
        return false;
    }
    return true;
}

void NvofMotion::stop() {
    if (engine_) {
        if (engine_->api.nvOFUnregisterResourceD3D11) {
            for (NvOFGPUBufferHandle& input : engine_->inputs) {
                if (input) engine_->api.nvOFUnregisterResourceD3D11(input);
                input = nullptr;
            }
            if (engine_->output) engine_->api.nvOFUnregisterResourceD3D11(engine_->output);
            engine_->output = nullptr;
        }
        if (engine_->session && engine_->api.nvOFDestroy) engine_->api.nvOFDestroy(engine_->session);
        engine_->session = nullptr;
    }
    for (int i = 0; i < 2; ++i) {
        inputTargets_[i].Reset();
        inputs_[i].Reset();
    }
    outputView_.Reset();
    output_.Reset();
    width_ = 0;
    height_ = 0;
}

bool NvofMotion::start(int width, int height) {
    // A session is set up once, for one size. So every start gets a new one.
    stop();
    const NV_OF_STATUS opened = engine_->api.nvCreateOpticalFlowD3D11(device_.Get(), context_.Get(), &engine_->session);
    if (opened != NV_OF_SUCCESS || !engine_->session) {
        engine_->session = nullptr;
        error_ = "the optical flow session could not be opened (status " + std::to_string(static_cast<int>(opened)) + ")";
        return false;
    }

    // Does the driver take the formats used here?
    const auto accepts = [&](NV_OF_BUFFER_USAGE usage, DXGI_FORMAT wanted) {
        if (!engine_->api.nvOFGetSurfaceFormatCountD3D11 || !engine_->api.nvOFGetSurfaceFormatD3D11) return true;
        uint32_t count = 0;
        if (engine_->api.nvOFGetSurfaceFormatCountD3D11(engine_->session, usage, NV_OF_MODE_OPTICALFLOW, &count) !=
                NV_OF_SUCCESS ||
            count == 0 || count > 64) {
            return false;
        }
        std::vector<DXGI_FORMAT> formats(count, DXGI_FORMAT_UNKNOWN);
        if (engine_->api.nvOFGetSurfaceFormatD3D11(engine_->session, usage, NV_OF_MODE_OPTICALFLOW, formats.data()) !=
            NV_OF_SUCCESS) {
            return false;
        }
        return std::find(formats.begin(), formats.end(), wanted) != formats.end();
    };
    if (!accepts(NV_OF_BUFFER_USAGE_INPUT, kInputFormat) || !accepts(NV_OF_BUFFER_USAGE_OUTPUT, kOutputFormat)) {
        error_ = "the optical flow engine does not take the picture formats used here";
        return false;
    }

    // One vector per four by four pixels is what every card with the engine
    // can do, and what is used. Should a card not list it, its finest grid is taken.
    grid_ = 4;
    if (engine_->api.nvOFGetCaps) {
        uint32_t size = 0;
        if (engine_->api.nvOFGetCaps(engine_->session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &size) ==
                NV_OF_SUCCESS &&
            size > 0 && size <= 16) {
            std::vector<uint32_t> sizes(size, 0);
            if (engine_->api.nvOFGetCaps(engine_->session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, sizes.data(), &size) ==
                NV_OF_SUCCESS) {
                sizes.resize(std::min<std::size_t>(size, sizes.size()));
                std::erase(sizes, 0u);
                if (!sizes.empty() && std::find(sizes.begin(), sizes.end(), 4u) == sizes.end()) {
                    grid_ = static_cast<int>(*std::min_element(sizes.begin(), sizes.end()));
                }
            }
        }
    }
    if (grid_ != 1 && grid_ != 2 && grid_ != 4) {
        error_ = "the optical flow engine offers no grid this program can use";
        return false;
    }

    NV_OF_INIT_PARAMS init;
    std::memset(&init, 0, sizeof(init));
    init.width = static_cast<uint32_t>(width);
    init.height = static_cast<uint32_t>(height);
    init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid_);
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.hPrivData = nullptr;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    const NV_OF_STATUS initialised = engine_->api.nvOFInit(engine_->session, &init);
    if (initialised != NV_OF_SUCCESS) {
        error_ = lastError("setting up the optical flow engine", static_cast<int>(initialised));
        return false;
    }

    cellsWide_ = (width + grid_ - 1) / grid_;
    cellsHigh_ = (height + grid_ - 1) / grid_;

    // Textures as the SDK's sample makes them, then handed to the engine.
    const auto make = [&](int w, int h, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& texture) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()));
    };
    for (int i = 0; i < 2; ++i) {
        if (!make(width, height, kInputFormat, inputs_[i]) ||
            FAILED(device_->CreateRenderTargetView(inputs_[i].Get(), nullptr, inputTargets_[i].GetAddressOf()))) {
            error_ = "the textures for the optical flow engine could not be created";
            return false;
        }
        const NV_OF_STATUS registered =
            engine_->api.nvOFRegisterResourceD3D11(engine_->session, inputs_[i].Get(), &engine_->inputs[i]);
        if (registered != NV_OF_SUCCESS) {
            error_ = lastError("handing a picture texture to the optical flow engine", static_cast<int>(registered));
            return false;
        }
    }
    if (!make(cellsWide_, cellsHigh_, kOutputFormat, output_) ||
        FAILED(device_->CreateShaderResourceView(output_.Get(), nullptr, outputView_.GetAddressOf()))) {
        error_ = "the texture for the motion vectors could not be created";
        return false;
    }
    const NV_OF_STATUS registered =
        engine_->api.nvOFRegisterResourceD3D11(engine_->session, output_.Get(), &engine_->output);
    if (registered != NV_OF_SUCCESS) {
        error_ = lastError("handing the vector texture to the optical flow engine", static_cast<int>(registered));
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

ID3D11RenderTargetView* NvofMotion::input(int slot, int width, int height) {
    if (!engine_ || slot < 0 || slot > 1 || width <= 0 || height <= 0) return nullptr;
    if (width != width_ || height != height_) {
        if (width == failedWidth_ && height == failedHeight_) return nullptr;
        if (!start(width, height)) {
            stop();
            failedWidth_ = width;
            failedHeight_ = height;
            return nullptr;
        }
        failedWidth_ = 0;
        failedHeight_ = 0;
    }
    return inputTargets_[slot].Get();
}

ID3D11ShaderResourceView* NvofMotion::motion(int slotFrom, int slotTo) {
    if (!engine_ || !engine_->session || width_ == 0 || slotFrom < 0 || slotFrom > 1 || slotTo < 0 || slotTo > 1 ||
        slotFrom == slotTo) {
        return nullptr;
    }
    NV_OF_EXECUTE_INPUT_PARAMS in;
    NV_OF_EXECUTE_OUTPUT_PARAMS out;
    std::memset(&in, 0, sizeof(in));
    std::memset(&out, 0, sizeof(out));
    // Each vector is the way of a part of `inputFrame` to its place in `referenceFrame`.
    in.inputFrame = engine_->inputs[slotFrom];
    in.referenceFrame = engine_->inputs[slotTo];
    // The pairs follow each other in time, so what the search found for the
    // pair before is a good start for this one.
    in.disableTemporalHints = NV_OF_FALSE;
    out.outputBuffer = engine_->output;
    const NV_OF_STATUS done = engine_->api.nvOFExecute(engine_->session, &in, &out);
    if (done != NV_OF_SUCCESS) {
        error_ = lastError("the motion search", static_cast<int>(done));
        return nullptr;
    }
    return outputView_.Get();
}

} // namespace veloplay::render
