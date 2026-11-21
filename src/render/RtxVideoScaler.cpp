#include "render/RtxVideoScaler.hpp"

#include <cstdio>

#include <dxgi.h>

namespace veloplay::render {

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kVendorNvidia = 0x10DE;

// NVIDIA's extension of the video processor. From Chromium, see the header.
constexpr GUID kNvidiaPpeInterface = {0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
struct NvidiaStreamExtension {
    UINT version; // 1
    UINT method;  // 2 = super resolution
    UINT enable;
};

std::string failure(const char* what, HRESULT result) {
    char text[160] = {};
    std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", what, static_cast<unsigned long>(result));
    return text;
}

} // namespace

bool RtxVideoScaler::create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& reason) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&description))) {
        reason = "the graphics adapter could not be identified";
        return false;
    }
    if (description.VendorId != kVendorNvidia) {
        reason = "the graphics card is not from NVIDIA";
        return false;
    }
    device_ = device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice_))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&videoContext_)))) {
        reason = "the graphics card offers no video processor";
        return false;
    }
    return true;
}

bool RtxVideoScaler::configure(int sourceWidth, int sourceHeight, int width, int height) {
    if (processor_ && sourceWidth == sourceWidth_ && sourceHeight == sourceHeight_ && width == width_ &&
        height == height_) {
        return true;
    }
    processor_.Reset();
    enumerator_.Reset();
    output_.Reset();
    outputView_.Reset();
    outputTarget_.Reset();
    inputs_.clear();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = static_cast<UINT>(sourceWidth);
    content.InputHeight = static_cast<UINT>(sourceHeight);
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(width);
    content.OutputHeight = static_cast<UINT>(height);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT result = videoDevice_->CreateVideoProcessorEnumerator(&content, enumerator_.GetAddressOf());
    if (FAILED(result)) {
        error_ = failure("CreateVideoProcessorEnumerator", result);
        return false;
    }
    UINT support = 0;
    if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_R8G8B8A8_UNORM, &support)) ||
        (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        error_ = "the video processor does not take RGBA pictures";
        return false;
    }
    result = videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, processor_.GetAddressOf());
    if (FAILED(result)) {
        error_ = failure("CreateVideoProcessor", result);
        return false;
    }

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = static_cast<UINT>(width);
    texture.Height = static_cast<UINT>(height);
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
    view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateTexture2D(&texture, nullptr, output_.GetAddressOf())) ||
        FAILED(device_->CreateShaderResourceView(output_.Get(), nullptr, outputView_.GetAddressOf())) ||
        FAILED(videoDevice_->CreateVideoProcessorOutputView(output_.Get(), enumerator_.Get(), &view,
                                                            outputTarget_.GetAddressOf()))) {
        error_ = "the texture for the enlarged picture could not be created";
        processor_.Reset();
        return false;
    }

    const RECT from{0, 0, sourceWidth, sourceHeight};
    const RECT to{0, 0, width, height};
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &from);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &to);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &to);
    // The driver's own "improvements" (noise reduction, colour tweaks) stay off; only what is asked for is applied.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    // In and out are the same kind of picture: RGB, full range, the usual transfer curve. Nothing is to be converted.
    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(videoContext_.As(&context1))) {
        context1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        context1->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    NvidiaStreamExtension extension{1, 2, 1};
    result = videoContext_->VideoProcessorSetStreamExtension(processor_.Get(), 0, &kNvidiaPpeInterface,
                                                             sizeof(extension), &extension);
    if (FAILED(result)) {
        error_ = failure("switching on RTX Video Super Resolution", result);
        processor_.Reset();
        return false;
    }
    sourceWidth_ = sourceWidth;
    sourceHeight_ = sourceHeight;
    width_ = width;
    height_ = height;
    return true;
}

ID3D11ShaderResourceView* RtxVideoScaler::scale(ID3D11Texture2D* source, int sourceWidth, int sourceHeight, int width,
                                                int height) {
    if (!videoDevice_ || !source || !configure(sourceWidth, sourceHeight, width, height)) return nullptr;

    ID3D11VideoProcessorInputView* input = nullptr;
    for (const auto& [texture, view] : inputs_) {
        if (texture == source) input = view.Get();
    }
    if (!input) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description{};
        description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> created;
        const HRESULT result =
            videoDevice_->CreateVideoProcessorInputView(source, enumerator_.Get(), &description, created.GetAddressOf());
        if (FAILED(result)) {
            error_ = failure("CreateVideoProcessorInputView", result);
            return nullptr;
        }
        if (inputs_.size() >= 8) inputs_.clear(); // textures were replaced; start over
        inputs_.emplace_back(source, created);
        input = created.Get();
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    const HRESULT result = videoContext_->VideoProcessorBlt(processor_.Get(), outputTarget_.Get(), 0, 1, &stream);
    if (FAILED(result)) {
        error_ = failure("VideoProcessorBlt", result);
        return nullptr;
    }
    return outputView_.Get();
}

} // namespace veloplay::render
