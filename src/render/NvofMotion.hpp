#pragma once

#include <string>

#include <d3d11.h>
#include <wrl/client.h>

namespace veloplay::render {

/// Finds out how things move between two pictures with the optical flow
/// engine of NVIDIA graphics cards (NVOFA). The engine is a piece of
/// hardware of its own on the card, made for exactly this search, and part
/// of the driver: `nvofapi64.dll` is loaded at run time and nothing is
/// shipped with the program. Cards from the RTX 20 series on have it.
///
/// Used through the interface NVIDIA publishes in its Optical Flow SDK
/// (third_party/nvof). Not used: NvOFFRUC, the SDK's ready-made frame
/// interpolation, which is closed and may not be passed on.
class NvofMotion {
public:
    NvofMotion();
    ~NvofMotion();
    NvofMotion(const NvofMotion&) = delete;
    NvofMotion& operator=(const NvofMotion&) = delete;

    /// False on cards of other makers, without the driver's library, or if
    /// the engine refuses the device. `error` then says which.
    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);

    /// Where the picture of a slot (0 or 1) has to be drawn before motion()
    /// is asked for. Sets the engine up for this size on the first call and
    /// when the size changes. Null if that failed; error() says why.
    ID3D11RenderTargetView* input(int slot, int width, int height);

    /// Runs the search from the picture in `slotFrom` to the one in
    /// `slotTo`. The result holds one vector per cell of grid() by grid()
    /// pixels, as two signed 16 bit numbers in 1/32 pixel: where that part
    /// of the first picture is in the second. Null if the engine failed.
    ID3D11ShaderResourceView* motion(int slotFrom, int slotTo);

    [[nodiscard]] int grid() const { return grid_; }
    [[nodiscard]] int cellsWide() const { return cellsWide_; }
    [[nodiscard]] int cellsHigh() const { return cellsHigh_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    struct Engine; // keeps NVIDIA's header out of this one

    bool start(int width, int height);
    void stop();
    std::string lastError(const char* what, int status);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Engine* engine_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputs_[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> inputTargets_[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputView_;
    int width_ = 0;
    int height_ = 0;
    int grid_ = 4;
    int cellsWide_ = 0;
    int cellsHigh_ = 0;
    int failedWidth_ = 0; // start() failed for this size; it is not tried again with every picture
    int failedHeight_ = 0;
    std::string error_;
};

} // namespace veloplay::render
