#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "media/FramePacer.hpp"
#include "media/VideoFrame.hpp"
#include "render/FrameGenerator.hpp"
#include "render/RtxVideoScaler.hpp"

namespace veloplay::render {

/// Brings decoded pictures to the screen with Direct3D 11, in three stages:
///
/// 1. Each picture from the console is converted from YUV to RGB once, into
///    a texture of its own size. The two newest are kept. Pictures decoded
///    on the graphics card are copied from the decoder's surface card to
///    card; pictures decoded on the CPU are uploaded.
/// 2. If a picture between those two is asked for, the frame generator
///    computes it.
/// 3. The chosen picture is scaled to the window, softly or with a Lanczos
///    filter, and sharpened if wanted.
class VideoRenderer {
public:
    /// What draw() is to show and how.
    struct Output {
        media::FrameChoice choice; // which picture; not valid means the newest
        bool upscale = false;      // Lanczos instead of soft enlargement
        bool rtx = false;          // with `upscale`: RTX Video Super Resolution where the card offers it,
                                   // also for a picture that keeps its size
        float sharpness = 0.0f;    // 0 = leave as it is, 1 = strongest
        bool deband = false;       // smooth the steps a low bitrate leaves in flat gradients
    };

    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);

    /// Takes over a picture. Returns false if it could not be brought onto the card.
    ///
    /// Every picture is also compared with the one before it, coarsely; the
    /// results come out of takeChange() a few pictures later. With `keep`
    /// false that is all that happens: the picture is measured and not shown.
    /// That is for a game at 30 fps in a 60 fps stream, whose every other
    /// picture repeats the one before.
    bool submit(const media::VideoFrame& frame, bool keep = true);

    /// How much of a picture differs from the one before it.
    struct Change {
        std::uint64_t picture = 0; // counted like submitted()
        double share = 0.0;        // part of the picture that changed visibly; 0 = none of it, 1 = all
        double mean = 0.0;         // mean difference in brightness levels, 0 to 255
    };
    /// The next finished comparison, oldest first.
    std::optional<Change> takeChange();
    /// Pictures handed to submit() so far, kept or not.
    [[nodiscard]] std::uint64_t submitted() const { return submitted_; }

    [[nodiscard]] bool hasPicture() const { return hasPicture_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    /// Number of the picture taken over last; they count up from 1.
    [[nodiscard]] std::uint64_t newestId() const { return slots_[newest_].id; }

    /// Draws into the bound render target, centred and with the aspect ratio
    /// kept. The area around the picture is left as it was cleared.
    void draw(UINT targetWidth, UINT targetHeight, const Output& output);
    void draw(UINT targetWidth, UINT targetHeight) { draw(targetWidth, targetHeight, Output{}); }

    /// False if the graphics card could not set frame generation up; the
    /// reason is in frameGeneratorError(). Pictures are then shown as they arrive.
    [[nodiscard]] bool frameGenerationAvailable() const { return generator_ != nullptr; }
    [[nodiscard]] const char* frameGeneratorName() const { return generator_ ? generator_->name() : ""; }
    [[nodiscard]] const std::string& frameGeneratorError() const { return generatorError_; }
    /// True while the motion between pictures is found by the graphics card's own engine (NVIDIA Optical Flow).
    [[nodiscard]] bool usesCardMotion() const { return generator_ && generator_->usesHardwareMotion(); }
    /// Why it is not; empty while it is.
    [[nodiscard]] std::string cardMotionError() const { return generator_ ? generator_->hardwareMotionError() : std::string{}; }
    /// Whether pictures in between will be asked for. While they are not,
    /// nothing is prepared for them: a 60 fps game shown as it is costs no
    /// motion search, and with frame generation switched off the card's
    /// engine is never touched.
    void wantGeneration(bool wanted) { generationWanted_ = wanted; }
    /// Switches the care for parts that stand still off or on, to see what it does.
    void holdStillParts(bool hold) {
        if (generator_) generator_->holdStillParts(hold);
        generatedFor_ = {};
    }
    /// Leaves the card's engine aside, to compare the two ways of finding motion.
    void allowCardMotion(bool allowed) {
        if (generator_) generator_->allowHardwareMotion(allowed);
        generatedFor_ = {};
    }
    /// True on NVIDIA cards with a video processor. Whether the driver really
    /// applies its network depends on NVIDIA's own settings.
    [[nodiscard]] bool rtxAvailable() const { return rtx_ != nullptr; }
    [[nodiscard]] const std::string& rtxError() const { return rtxError_; }
    /// What the last draw() put the picture through on its way to the window:
    /// "RTX Video", "Lanczos" or, for the card's plain filter, nothing.
    [[nodiscard]] const char* scalerName() const { return scalerName_; }
    /// Whether the last draw smoothed banding.
    [[nodiscard]] bool debanded() const { return debanded_.used; }
    /// Size of the picture on screen at the last draw(), in pixels; 0 before the first.
    [[nodiscard]] int shownWidth() const { return shownWidth_; }
    [[nodiscard]] int shownHeight() const { return shownHeight_; }
    /// True if the last draw() showed a computed picture.
    [[nodiscard]] bool lastDrawWasGenerated() const { return lastGenerated_; }

    /// Forgets the pictures, for example when a session ends.
    void clear();

private:
    struct Target {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        int width = 0;
        int height = 0;
    };
    struct Slot {
        Target rgb;
        std::uint64_t id = 0; // 0 = empty
    };

    bool ensureTextures(int width, int height, bool onCard);
    bool ensureTarget(Target& target, int width, int height);
    bool upload(const media::VideoFrame& frame);
    void pass(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport,
              ID3D11ShaderResourceView* const* views, UINT viewCount, ID3D11Buffer* constants);
    ID3D11ShaderResourceView* pictureFor(const media::FrameChoice& choice);
    void probe();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> convertShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> softShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lanczosShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> sharpenShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> debandShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> colorConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> outputConstants_;

    // CPU path: two textures. Card path: one NV12 texture seen through two views.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lumaTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> chromaTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaView_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaView_;

    Slot slots_[2];        // the two newest pictures, as RGB
    int newest_ = 0;       // which of the two is the newer one
    std::uint64_t nextId_ = 1;
    Target generated_;     // the picture computed last
    media::FrameChoice generatedFor_{};
    Target scaled_;        // the picture at window size, when sharpening needs one
    struct {
        Target target;     // the picture with its banding smoothed, at the stream's size
        int seed = 0;
        bool used = false; // the last draw went through it
    } debanded_;
    std::unique_ptr<IFrameGenerator> generator_;
    std::string generatorError_;
    bool generationWanted_ = true;
    bool prepared_[2] = {false, false}; // the generator has seen the picture in this slot
    std::unique_ptr<RtxVideoScaler> rtx_;
    std::string rtxError_;
    const char* scalerName_ = "";
    bool lastGenerated_ = false;
    int shownWidth_ = 0;
    int shownHeight_ = 0;

    // The comparison of each picture with the one before. A small copy of
    // its brightness is drawn, copied to a texture the CPU can read, and
    // read three pictures later, when the card is certainly done with it:
    // reading at once would make the whole pipeline wait for the card.
    static constexpr int kProbeWidth = 96;
    static constexpr int kProbeHeight = 54;
    struct ProbeCopy {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        std::uint64_t picture = 0;
        bool pending = false;
    };
    Microsoft::WRL::ComPtr<ID3D11PixelShader> probeShader_;
    Target probeTarget_;
    ProbeCopy probeCopies_[3];
    std::vector<std::uint8_t> probeBefore_; // the small copy of the picture read last
    std::uint64_t probeBeforePicture_ = 0;
    std::deque<Change> changes_;
    std::uint64_t submitted_ = 0;

    int width_ = 0;
    int height_ = 0;
    bool onCard_ = false;
    bool hasPicture_ = false;
    media::ColorInfo color_{};
    bool colorSet_ = false;
};

} // namespace veloplay::render
