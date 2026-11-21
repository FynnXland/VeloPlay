#pragma once

#include <cstdint>
#include <memory>
#include <string>

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
        bool rtx = false;          // with `upscale`: RTX Video Super Resolution where the card offers it
        float sharpness = 0.0f;    // 0 = leave as it is, 1 = strongest
    };

    bool create(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);

    /// Takes over a picture. Returns false if it could not be brought onto the card.
    bool submit(const media::VideoFrame& frame);

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
    /// True on NVIDIA cards with a video processor. Whether the driver really
    /// applies its network depends on NVIDIA's own settings.
    [[nodiscard]] bool rtxAvailable() const { return rtx_ != nullptr; }
    [[nodiscard]] const std::string& rtxError() const { return rtxError_; }
    /// How the last draw() enlarged the picture: "RTX Video", "Lanczos" or empty.
    [[nodiscard]] const char* scalerName() const { return scalerName_; }
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

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> convertShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> softShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lanczosShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> sharpenShader_;
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
    std::unique_ptr<IFrameGenerator> generator_;
    std::string generatorError_;
    std::unique_ptr<RtxVideoScaler> rtx_;
    std::string rtxError_;
    const char* scalerName_ = "";
    bool lastGenerated_ = false;
    int shownWidth_ = 0;
    int shownHeight_ = 0;

    int width_ = 0;
    int height_ = 0;
    bool onCard_ = false;
    bool hasPicture_ = false;
    media::ColorInfo color_{};
    bool colorSet_ = false;
};

} // namespace veloplay::render
