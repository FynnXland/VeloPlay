#pragma once

#include <string>
#include <utility>
#include <vector>

#include <d3d11_1.h>
#include <wrl/client.h>

namespace veloplay::render {

/// Enlarges pictures with NVIDIA's RTX Video Super Resolution: a neural
/// network in the driver that removes compression blocks and restores edges
/// while scaling. Only on NVIDIA cards that offer it (RTX 20 series and up).
///
/// The way in is Direct3D 11's video processor, which every card has, plus a
/// switch that only NVIDIA's driver understands. The switch is not in any
/// Microsoft header; identifier and layout are the ones Chromium uses
/// (ui/gl/swap_chain_presenter.cc) and mpv took over (video/filter/vf_d3d11vpp.c).
///
/// What cannot be found out from here: whether the driver really applies the
/// network. It does so only if "RTX Video Enhancement" is switched on in
/// NVIDIA's settings, and it says nothing when it is not. Without it the
/// picture is still scaled, by the video processor's ordinary filter.
class RtxVideoScaler {
public:
    /// False, with a reason, on cards from other makers or without a video processor.
    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& reason);

    /// Scales `source`, an RGBA texture of sourceWidth by sourceHeight that
    /// can be rendered to, to width by height. Returns the result, or null
    /// if the driver refused; error() then says why.
    ID3D11ShaderResourceView* scale(ID3D11Texture2D* source, int sourceWidth, int sourceHeight, int width, int height);

    /// With false the picture goes through the same video processor, but
    /// NVIDIA's extension is never switched on: the driver's ordinary filter
    /// enlarges it. Comparing the two results is the only way to tell whether
    /// the driver applies its network at all.
    void useNetwork(bool use) {
        if (use == network_) return;
        network_ = use;
        processor_.Reset(); // a processor that once had the extension is not trusted to forget it
    }

    [[nodiscard]] const std::string& error() const { return error_; }

private:
    bool configure(int sourceWidth, int sourceHeight, int width, int height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputView_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputTarget_;
    // One input view per texture that was handed in; there are three at most.
    std::vector<std::pair<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>>> inputs_;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool network_ = true;
    std::string error_;
};

} // namespace veloplay::render
