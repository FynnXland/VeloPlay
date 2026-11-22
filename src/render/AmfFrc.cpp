#include "render/AmfFrc.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include <spdlog/spdlog.h>

#include "components/FRC.h"
#include "core/Factory.h"

namespace veloplay::render {

using Microsoft::WRL::ComPtr;

namespace {

std::string result(const char* what, AMF_RESULT code) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s failed (AMF %d)", what, static_cast<int>(code));
    return text;
}

} // namespace

struct AmfFrcGenerator::Amf {
    HMODULE library = nullptr;
    amf::AMFFactory* factory = nullptr; // belongs to the runtime; not released
    amf::AMFContext* context = nullptr;
    amf::AMFComponent* frc = nullptr;

    ~Amf() {
        if (frc) {
            frc->Terminate();
            frc->Release();
        }
        if (context) {
            context->Terminate();
            context->Release();
        }
        // The runtime stays loaded: AMD's own samples never unload it while objects may still be around.
    }
};

AmfFrcGenerator::AmfFrcGenerator(std::unique_ptr<IFrameGenerator> inner) : inner_(std::move(inner)) {}

AmfFrcGenerator::~AmfFrcGenerator() = default;

std::unique_ptr<IFrameGenerator> AmfFrcGenerator::release() {
    amf_.reset();
    return std::move(inner_);
}

bool AmfFrcGenerator::create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error) {
    context_ = context;
    auto amf = std::make_unique<Amf>();
    // From the system folder only, where AMD's driver puts it.
    amf->library = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!amf->library) {
        error = "AMF is not installed (it comes with the driver of an AMD graphics card)";
        return false;
    }
    const auto init = reinterpret_cast<AMFInit_Fn>(reinterpret_cast<void*>(GetProcAddress(amf->library, AMF_INIT_FUNCTION_NAME)));
    if (!init) {
        error = "AMF lacks its entry point";
        return false;
    }
    AMF_RESULT code = init(AMF_FULL_VERSION, &amf->factory);
    if (code != AMF_OK || !amf->factory) {
        error = result("starting AMF", code);
        return false;
    }
    code = amf->factory->CreateContext(&amf->context);
    if (code != AMF_OK) {
        error = result("creating an AMF context", code);
        return false;
    }
    code = amf->context->InitDX11(device);
    if (code != AMF_OK) {
        error = result("tying AMF to the graphics device", code);
        return false;
    }
    code = amf->factory->CreateComponent(amf->context, AMFFRC, &amf->frc);
    if (code != AMF_OK || !amf->frc) {
        error = result("creating FRC (it needs a recent AMD card and driver)", code);
        return false;
    }
    amf::AMFComponent* frc = amf->frc;
    frc->SetProperty(AMF_FRC_ENGINE_TYPE, static_cast<amf_int64>(FRC_ENGINE_DX11));
    frc->SetProperty(AMF_FRC_MODE, static_cast<amf_int64>(FRC_ONLY_INTERPOLATED));
    // The header's default waits for the picture after next: a whole picture more delay.
    frc->SetProperty(AMF_FRC_USE_FUTURE_FRAME, false);
    frc->SetProperty(AMF_FRC_INDICATOR, false);
    frc->SetProperty(AMF_FRC_ENABLE_FALLBACK, true);
    frc->SetProperty(AMF_FRC_PROFILE, static_cast<amf_int64>(FRC_PROFILE_HIGH));
    amf_ = std::move(amf);
    spdlog::info("Frame generation: AMD FRC is ready");
    return true;
}

void AmfFrcGenerator::fail(std::string why) {
    if (!failure_.empty()) return;
    failure_ = std::move(why);
    spdlog::warn("Frame generation: AMD FRC is not used any more: {}", failure_);
    amf_.reset();
    middle_.Reset();
}

bool AmfFrcGenerator::prepare(int slot, ID3D11ShaderResourceView* picture, int width, int height) {
    // The wrapped method prepares every picture as well: it computes 3x and 4x, and takes over if FRC gives up.
    const bool innerReady = inner_->prepare(slot, picture, width, height);
    if (!failure_.empty() || !amf_) return innerReady;

    if (!initialised_ || width != width_ || height != height_) {
        const AMF_RESULT code = initialised_ ? amf_->frc->ReInit(width, height)
                                             : amf_->frc->Init(amf::AMF_SURFACE_RGBA, width, height);
        if (code != AMF_OK) {
            fail(result("setting FRC up for the picture size", code));
            return innerReady;
        }
        initialised_ = true;
        width_ = width;
        height_ = height;
        middle_.Reset();
        lastSlot_ = -1;
    }

    ComPtr<ID3D11Resource> resource;
    picture->GetResource(resource.GetAddressOf());
    amf::AMFSurface* surface = nullptr;
    AMF_RESULT code = amf_->context->CreateSurfaceFromDX11Native(resource.Get(), &surface, nullptr);
    if (code != AMF_OK || !surface) {
        fail(result("handing a picture to FRC", code));
        return innerReady;
    }
    // A picture lasts 1000 units here; FRC's picture in between gets a stamp from the pair it was made of.
    const long long stamp = ++sequence_ * 1000;
    surface->SetPts(stamp);
    surface->SetDuration(1000);
    code = amf_->frc->SubmitInput(surface);
    surface->Release();
    if (code != AMF_OK && code != AMF_NEED_MORE_INPUT) {
        fail(result("FRC taking a picture", code));
        return innerReady;
    }
    const int previous = lastSlot_;
    const long long previousStamp = previousStamp_;
    lastSlot_ = slot;
    previousStamp_ = stamp;
    // After the second picture FRC has the one in between: from the picture before to this one.
    amf::AMFData* data = nullptr;
    code = amf_->frc->QueryOutput(&data);
    if (code == AMF_REPEAT || code == AMF_NEED_MORE_INPUT || code == AMF_OK) {
        if (!data) {
            middleFrom_ = middleTo_ = -1;
            return innerReady;
        }
    } else {
        if (data) data->Release();
        fail(result("FRC giving the picture in between", code));
        return innerReady;
    }
    amf::AMFSurface* out = nullptr;
    const AMF_RESULT queried = data->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&out));
    data->Release();
    if (queried != AMF_OK || !out || out->GetPlanesCount() == 0) {
        if (out) out->Release();
        fail("FRC gave back no picture");
        return innerReady;
    }
    // Only a picture made of exactly this pair: its stamp lies between the two pictures', or on the newer one.
    const long long made = out->GetPts();
    if (previousStamp < 0 || made <= previousStamp || made > stamp) {
        out->Release();
        middleFrom_ = middleTo_ = -1;
        return innerReady;
    }
    auto* texture = static_cast<ID3D11Texture2D*>(out->GetPlaneAt(0)->GetNative());
    if (!middle_) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Device> device;
        context_->GetDevice(device.GetAddressOf());
        if (FAILED(device->CreateTexture2D(&desc, nullptr, middle_.GetAddressOf()))) {
            out->Release();
            fail("no memory for FRC's picture");
            return innerReady;
        }
    }
    // FRC's own surface goes back to it; a copy waits until the picture in between is asked for.
    context_->CopyResource(middle_.Get(), texture);
    out->Release();
    middleFrom_ = previous;
    middleTo_ = slot;
    return innerReady;
}

bool AmfFrcGenerator::generate(int slotFrom, ID3D11ShaderResourceView* from, int slotTo, ID3D11ShaderResourceView* to,
                               float phase, ID3D11RenderTargetView* target) {
    const bool middle = std::abs(phase - 0.5f) < 0.01f;
    if (failure_.empty() && middle && middle_ && slotFrom == middleFrom_ && slotTo == middleTo_) {
        ComPtr<ID3D11Resource> into;
        target->GetResource(into.GetAddressOf());
        D3D11_TEXTURE2D_DESC have{};
        D3D11_TEXTURE2D_DESC want{};
        middle_->GetDesc(&have);
        ComPtr<ID3D11Texture2D> targetTexture;
        if (SUCCEEDED(into.As(&targetTexture))) targetTexture->GetDesc(&want);
        if (have.Width == want.Width && have.Height == want.Height && have.Format == want.Format) {
            context_->CopyResource(into.Get(), middle_.Get());
            return true;
        }
        fail("FRC's picture has another format than VeloPlay's");
    }
    return inner_->generate(slotFrom, from, slotTo, to, phase, target);
}

} // namespace veloplay::render
