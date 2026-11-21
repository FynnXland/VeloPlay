#pragma once

#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace veloplay::render {

class NvofMotion;

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

    /// True while the graphics card's own motion search is in use.
    [[nodiscard]] virtual bool usesHardwareMotion() const { return false; }
    /// Lets a caller leave the card's motion search aside, to compare the two ways.
    virtual void allowHardwareMotion(bool allowed) { (void)allowed; }
    /// Why the card's motion search is not in use; empty if it is, or was never an option.
    [[nodiscard]] virtual std::string hardwareMotionError() const { return {}; }
    /// Lets a caller switch off the care for parts that stand still, to see what it does.
    virtual void holdStillParts(bool hold) { (void)hold; }
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
///
/// Two things are looked after on top of that:
///
/// - A cut. When the game jumps to another picture, nothing in the one
///   matches anything in the other, and a mix of both shows as a flash of
///   two pictures at once. If the fine structure of about half the picture
///   fits neither along the shift that was found nor in place, the nearer
///   of the two real pictures is shown instead. A flash or a fade is no
///   cut: there the structure stays where it is.
/// - What stands still for a while, above all the game's own display: a
///   map, a counter, writing. The scene moves behind it, the search follows
///   the scene, and the edges of the writing would be dragged along and
///   shimmer. Every pixel keeps count of how long it has been unchanged;
///   edges that have stood for about a second are taken from the real
///   pictures as they are.
///
/// On NVIDIA cards step 2 is done by the card's optical flow engine instead
/// (NvofMotion): hardware made for this search, which also gets right what a
/// search from coarse to fine gets wrong, like the rungs of a ladder that
/// all look the same. Its vectors say where each part of the earlier
/// picture goes; they are carried over to the moment in between, and each
/// place then picks, among the vectors around it, the one that fits the two
/// pictures best. Step 3 is the same. If the engine is missing or fails,
/// the built-in search takes over.
class FlowFrameGenerator final : public IFrameGenerator {
public:
    FlowFrameGenerator();
    ~FlowFrameGenerator() override;

    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);

    [[nodiscard]] const char* name() const override { return usesHardwareMotion() ? "NVIDIA Optical Flow" : "VeloPlay Flow"; }
    [[nodiscard]] bool usesHardwareMotion() const override { return nvof_ != nullptr && nvofAllowed_; }
    void allowHardwareMotion(bool allowed) override { nvofAllowed_ = allowed; }
    void holdStillParts(bool hold) override { holdStill_ = hold; }
    [[nodiscard]] std::string hardwareMotionError() const override { return nvofError_; }
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
    bool makeVectors(Level& level, int width, int height);
    bool hardwareFlow(int slotFrom, int slotTo, float phase, ID3D11ShaderResourceView*& flow);
    bool finish(int slotFrom, ID3D11ShaderResourceView* from, int slotTo, ID3D11ShaderResourceView* to,
                ID3D11ShaderResourceView* flow, float phase, ID3D11RenderTargetView* target);
    void dropHardwareMotion(const std::string& reason);
    void pass(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, int width, int height,
              ID3D11ShaderResourceView* const* views, UINT viewCount);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lumaShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> flowShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> synthesisShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
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

    // Cuts: how much of the picture found no fit, boiled down to one number the last pass can read.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> reduceShader_;
    Level misfitCoarse_; // eight by eight
    Level misfit_;       // one by one
    // Standing still: per pixel how long it has been unchanged and how much of an edge it is.
    struct Still {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    };
    Microsoft::WRL::ComPtr<ID3D11PixelShader> stillShader_;
    Still still_[2];       // written in turns: one is read while the other is written
    int stillNow_ = 0;     // the one that holds the newest count
    bool pairIsNew_ = false; // a picture arrived since the count was last brought up to date
    bool holdStill_ = true;

    // The card's own motion search, where there is one.
    std::unique_ptr<NvofMotion> nvof_;
    bool nvofAllowed_ = true;
    std::string nvofError_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> projectShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> chooseShader_;
    Level carried_;             // the card's vectors, carried over to the moment in between
    Level chosen_;              // per place the best of the vectors around it
    bool nvofInput_[2] = {false, false}; // the engine has the picture of this slot
    ID3D11ShaderResourceView* nvofVectors_ = nullptr; // result of the search for the pair in the slots
    int nvofFrom_ = -1;         // which pair that result belongs to
    int nvofTo_ = -1;
};

} // namespace veloplay::render
