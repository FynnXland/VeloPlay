#pragma once

#include <string>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace veloplay::render {

/// Direct3D 11 device with a flip-model swap chain for one window.
///
/// Flip model (DXGI_SWAP_EFFECT_FLIP_DISCARD) is what allows tearing-free
/// presentation without the compositor copying every frame, and tearing
/// (variable refresh rate) when VSync is off.
class D3D11Device {
public:
    D3D11Device() = default;
    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    /// Returns false if no usable graphics adapter was found; `error` then says why.
    bool create(HWND window, std::string& error);

    /// Call after the window changed size. Zero sizes (minimised) are ignored.
    void resize(UINT width, UINT height);

    /// Binds the back buffer and clears it.
    void beginFrame(const float clearColor[4]);

    /// Waits until the display can take the next frame, at most `timeoutMs`.
    /// Call before drawing. Waiting here instead of inside present() matters:
    /// present() would wait while holding the device's lock, and the video
    /// decoder, which shares the device, would stand still for that long.
    void waitForFrame(unsigned timeoutMs);

    /// Returns false when the device was lost (driver update, GPU removed).
    bool present(bool vsync);

    [[nodiscard]] ID3D11Device* device() const { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const { return context_.Get(); }
    [[nodiscard]] const std::string& adapterName() const { return adapterName_; }
    [[nodiscard]] bool tearingSupported() const { return tearingSupported_; }
    [[nodiscard]] UINT width() const { return width_; }
    [[nodiscard]] UINT height() const { return height_; }

private:
    bool createRenderTarget();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    std::string adapterName_;
    bool tearingSupported_ = false;
    UINT swapChainFlags_ = 0;
    HANDLE frameLatencyWait_ = nullptr; // owned by the swap chain
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace veloplay::render
