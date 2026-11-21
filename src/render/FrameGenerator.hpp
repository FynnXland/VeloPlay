#pragma once

#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace veloplay::render {

/// Computes a picture that lies between two pictures from the console.
/// One interface, several ways to do it: the built-in one below runs on any
/// Direct3D 11 card; ones that use a vendor's hardware can follow.
class IFrameGenerator {
public:
    virtual ~IFrameGenerator() = default;

    /// Short name for the display, for example "VeloPlay Flow".
    [[nodiscard]] virtual const char* name() const = 0;

    /// A picture from the console went into `slot` (0 or 1). Called once per
    /// picture, so work that belongs to a picture and not to a pair is done
    /// only once.
    virtual bool prepare(int slot, ID3D11ShaderResourceView* picture, int width, int height) = 0;

    /// Writes the picture at `phase` (0 = `from`, 1 = `to`) into `target`,
    /// which has the size of the pictures. Leaves no render target bound.
    virtual bool generate(int slotFrom, ID3D11ShaderResourceView* from, int slotTo, ID3D11ShaderResourceView* to,
                          float phase, ID3D11RenderTargetView* target) = 0;
};

/// The built-in method. It follows how things move between the two pictures
/// and puts each part of the new picture where it would be at that moment:
///
/// 1. Both pictures are reduced to brightness at half size, with ever
///    smaller copies below that.
/// 2. Starting on the smallest copies, each block looks for the shift at
///    which the two pictures match best, going out from what the level above
///    found. Shifts are tried around the moment in between, so the result
///    says where things are in the new picture, not in one of the old ones.
/// 3. Every pixel of the new picture is fetched from both pictures along
///    that shift and mixed. Where the two disagree, because something came
///    into view or the search was wrong, it falls back to a plain mix.
///
/// Pixel shaders only, so it runs on every Direct3D 11 card.
class FlowFrameGenerator final : public IFrameGenerator {
public:
    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);

    [[nodiscard]] const char* name() const override { return "VeloPlay Flow"; }
    bool prepare(int slot, ID3D11ShaderResourceView* picture, int width, int height) override;
    bool generate(int slotFrom, ID3D11ShaderResourceView* from, int slotTo, ID3D11ShaderResourceView* to, float phase,
                  ID3D11RenderTargetView* target) override;

private:
    struct Level {
        int lumaWidth = 0; // size of the brightness copy on this level
        int lumaHeight = 0;
        int flowWidth = 0; // one shift per block of four by four of its pixels
        int flowHeight = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> flow;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flowView;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> flowTarget;
    };

    bool ensureSize(int width, int height);
    bool makeFlow(Level& level);
    void pass(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, int width, int height,
              ID3D11ShaderResourceView* const* views, UINT viewCount);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lumaShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> flowShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> synthesisShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

    int width_ = 0;
    int height_ = 0;
    // Brightness at half size with all smaller copies, one per picture slot.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> luma_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaView_[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> lumaTarget_[2];
    std::vector<Level> levels_; // index 0 is the finest
    Level refined_;             // the finest level once more, with half steps
};

} // namespace veloplay::render
