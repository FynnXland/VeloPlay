#include "render/D3D11Device.hpp"

#include <cstdio>

#include <spdlog/spdlog.h>

namespace veloplay::render {

namespace {

using Microsoft::WRL::ComPtr;

std::string toUtf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string hresult(const char* what, HRESULT hr) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buffer;
}

/// On machines with two graphics chips the discrete one decodes and scales
/// video faster, so ask Windows for the high-performance adapter first.
ComPtr<IDXGIAdapter1> pickAdapter(IDXGIFactory2* factory) {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
        for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                              IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
            adapter.Reset();
        }
    }
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
        adapter.Reset();
    }
    return nullptr;
}

} // namespace

bool D3D11Device::create(HWND window, std::string& error) {
    destroy();
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = hresult("CreateDXGIFactory1", hr);
        return false;
    }

    const ComPtr<IDXGIAdapter1> adapter = pickAdapter(factory.Get());
    if (adapter) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        adapterName_ = toUtf8(desc.Description);
        adapterVendor_ = desc.VendorId;
        dedicatedVideoMemory_ = static_cast<unsigned long long>(desc.DedicatedVideoMemory);
    }
    softwareRenderer_ = false;

    // VIDEO_SUPPORT is required later for hardware video decoding (D3D11VA).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};

    const auto createDevice = [&](IDXGIAdapter* target, D3D_DRIVER_TYPE type, UINT deviceFlags) {
        return D3D11CreateDevice(target, type, nullptr, deviceFlags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                 &device_, &obtained, &context_);
    };

    // With an explicit adapter the driver type has to be UNKNOWN.
    hr = adapter ? createDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, flags)
                 : createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, flags);
    if (FAILED(hr)) {
        // No hardware device: the software rasteriser still shows the UI, which
        // is enough to pair a console or read an error message.
        spdlog::warn("{}; falling back to the software renderer", hresult("D3D11CreateDevice", hr));
        hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
        adapterName_ = "Microsoft Basic Render Driver";
        adapterVendor_ = 0;
        dedicatedVideoMemory_ = 0;
        softwareRenderer_ = true;
    }
    if (FAILED(hr)) {
        error = hresult("D3D11CreateDevice", hr);
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                    sizeof(allowTearing)))) {
            tearingSupported_ = allowTearing == TRUE;
        }
    }

    // The swap chain has to come from the factory that owns the device's adapter.
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> deviceAdapter;
    ComPtr<IDXGIFactory2> deviceFactory;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&deviceAdapter)) ||
        FAILED(deviceAdapter->GetParent(IID_PPV_ARGS(&deviceFactory)))) {
        error = "could not find the DXGI factory of the graphics device";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // The waitable object tells when the display is ready for a frame, so
    // that present() itself never has to wait (see waitForFrame()).
    swapChainFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                      (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    desc.Flags = swapChainFlags_;

    hr = deviceFactory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) {
        // Very old systems do not know the waitable object.
        swapChainFlags_ &= ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        desc.Flags = swapChainFlags_;
        hr = deviceFactory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapChain_);
    }
    if (FAILED(hr)) {
        error = hresult("CreateSwapChainForHwnd", hr);
        return false;
    }
    if (swapChainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        ComPtr<IDXGISwapChain2> waitable;
        if (SUCCEEDED(swapChain_.As(&waitable))) {
            waitable->SetMaximumFrameLatency(1);
            frameLatencyWait_ = waitable->GetFrameLatencyWaitableObject();
        }
    }
    deviceFactory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    DXGI_SWAP_CHAIN_DESC1 actual{};
    swapChain_->GetDesc1(&actual);
    width_ = actual.Width;
    height_ = actual.Height;

    spdlog::info("Graphics: {} (feature level {:x}, tearing {})", adapterName_, static_cast<unsigned>(obtained),
                 tearingSupported_ ? "supported" : "not supported");
    if (!createRenderTarget()) {
        error = "could not create the render target";
        return false;
    }
    return true;
}

void D3D11Device::destroy() {
    // Direct3D 11 destroys objects late, once the context lets go of them. The device itself may
    // outlive this (the old decoder still holds it), so its swap chain has to go explicitly:
    // every reference released first, then ClearState() and Flush(), which is Microsoft's order.
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    if (frameLatencyWait_) {
        CloseHandle(frameLatencyWait_);
        frameLatencyWait_ = nullptr;
    }
    swapChain_.Reset();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
    device_.Reset();
    adapterName_.clear();
    adapterVendor_ = 0;
    dedicatedVideoMemory_ = 0;
    softwareRenderer_ = false;
    tearingSupported_ = false;
    swapChainFlags_ = 0;
    width_ = 0;
    height_ = 0;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11Device::backBuffer() const {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
    if (swapChain_) swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
    return buffer;
}

bool D3D11Device::createRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_));
}

void D3D11Device::resize(UINT width, UINT height) {
    if (!swapChain_ || width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    // Every reference to the old buffers has to go before they can be resized.
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (FAILED(hr)) {
        spdlog::error("{}", hresult("ResizeBuffers", hr));
        return;
    }
    width_ = width;
    height_ = height;
    createRenderTarget();
}

void D3D11Device::beginFrame(const float clearColor[4]) {
    if (!renderTarget_) return;
    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->ClearRenderTargetView(target, clearColor);
}

void D3D11Device::waitForFrame(unsigned timeoutMs) {
    if (frameLatencyWait_) WaitForSingleObjectEx(frameLatencyWait_, timeoutMs, TRUE);
}

bool D3D11Device::present(bool vsync) {
    if (!swapChain_) return false;
    // Tearing is only legal with a sync interval of zero.
    const UINT flags = (!vsync && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = swapChain_->Present(vsync ? 1 : 0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // The removed reason tells more than the result: hung, reset, driver error, card gone.
        const HRESULT reason = hr == DXGI_ERROR_DEVICE_REMOVED ? device_->GetDeviceRemovedReason() : hr;
        lostReason_ = hresult("Present", reason);
        spdlog::error("{}", lostReason_);
        return false;
    }
    return true;
}

} // namespace veloplay::render
