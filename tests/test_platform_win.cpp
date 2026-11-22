// Windows only. Draws with WARP, the software renderer every Windows has, so
// the shader and the upload paths are checked on machines without a graphics card.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "audio/WasapiOutput.hpp"
#include "render/RtxVideoScaler.hpp"
#include "render/VideoRenderer.hpp"
#include "util/ThreadStack.hpp"

using namespace veloplay;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kTarget = 64;

struct Canvas {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11Texture2D> staging;

    bool create() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                     device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kTarget;
        desc.Height = kTarget;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf()))) return false;
        if (FAILED(device->CreateRenderTargetView(target.Get(), nullptr, view.GetAddressOf()))) return false;
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()));
    }

    void begin() {
        const float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        ID3D11RenderTargetView* targets[1] = {view.Get()};
        context->OMSetRenderTargets(1, targets, nullptr);
        context->ClearRenderTargetView(view.Get(), gray);
    }

    /// RGBA of one pixel of what was drawn.
    std::array<int, 4> pixel(UINT x, UINT y) {
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        std::array<int, 4> out{-1, -1, -1, -1};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;
        const auto* p = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
        out = {p[0], p[1], p[2], p[3]};
        context->Unmap(staging.Get(), 0);
        return out;
    }
};

/// A picture of one colour in CPU memory.
struct SolidFrame {
    std::vector<std::uint8_t> luma, u, v, uv;
    media::VideoFrame frame;

    SolidFrame(int width, int height, int y, int cb, int cr, bool nv12) {
        const int cw = (width + 1) / 2;
        const int ch = (height + 1) / 2;
        luma.assign(static_cast<std::size_t>(width * height), static_cast<std::uint8_t>(y));
        frame.width = width;
        frame.height = height;
        frame.planes[0] = luma.data();
        frame.strides[0] = width;
        if (nv12) {
            uv.resize(static_cast<std::size_t>(cw * ch * 2));
            for (std::size_t i = 0; i < uv.size(); i += 2) {
                uv[i] = static_cast<std::uint8_t>(cb);
                uv[i + 1] = static_cast<std::uint8_t>(cr);
            }
            frame.layout = media::PixelLayout::Nv12;
            frame.planes[1] = uv.data();
            frame.strides[1] = cw * 2;
        } else {
            u.assign(static_cast<std::size_t>(cw * ch), static_cast<std::uint8_t>(cb));
            v.assign(static_cast<std::size_t>(cw * ch), static_cast<std::uint8_t>(cr));
            frame.layout = media::PixelLayout::I420;
            frame.planes[1] = u.data();
            frame.planes[2] = v.data();
            frame.strides[1] = cw;
            frame.strides[2] = cw;
        }
    }
};

} // namespace

TEST(VideoRenderer, ConvertsAndLetterboxesACpuPicture) {
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;
    EXPECT_FALSE(renderer.hasPicture());

    // Nothing to draw yet: the target keeps its clear colour.
    canvas.begin();
    renderer.draw(kTarget, kTarget);
    EXPECT_NEAR(canvas.pixel(32, 32)[0], 128, 2);

    // Pure red in BT.709 with limited range, twice as wide as high.
    SolidFrame red(32, 16, 63, 102, 240, false);
    ASSERT_TRUE(renderer.submit(red.frame));
    EXPECT_TRUE(renderer.hasPicture());
    canvas.begin();
    renderer.draw(kTarget, kTarget);

    const auto centre = canvas.pixel(32, 32);
    EXPECT_NEAR(centre[0], 255, 4);
    EXPECT_NEAR(centre[1], 0, 4);
    EXPECT_NEAR(centre[2], 0, 4);
    // The picture fills the width and the middle half of the height.
    EXPECT_NEAR(canvas.pixel(2, 32)[0], 255, 4);
    EXPECT_NEAR(canvas.pixel(61, 32)[0], 255, 4);
    EXPECT_NEAR(canvas.pixel(32, 18)[0], 255, 4);
    EXPECT_NEAR(canvas.pixel(32, 45)[0], 255, 4);
    // Above and below it the target is untouched.
    EXPECT_NEAR(canvas.pixel(32, 8)[0], 128, 2);
    EXPECT_NEAR(canvas.pixel(32, 8)[1], 128, 2);
    EXPECT_NEAR(canvas.pixel(32, 56)[1], 128, 2);
}

TEST(VideoRenderer, TakesNv12AndFollowsFormatChanges) {
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;

    // White, as a portrait picture in the NV12 layout.
    SolidFrame white(16, 32, 235, 128, 128, true);
    ASSERT_TRUE(renderer.submit(white.frame));
    canvas.begin();
    renderer.draw(kTarget, kTarget);
    const auto centre = canvas.pixel(32, 32);
    EXPECT_NEAR(centre[0], 255, 3);
    EXPECT_NEAR(centre[1], 255, 3);
    EXPECT_NEAR(centre[2], 255, 3);
    EXPECT_NEAR(canvas.pixel(4, 32)[0], 128, 2); // bars left and right this time

    // The stream switches to another size and to full-range BT.601: blue.
    SolidFrame blue(48, 48, 29, 255, 107, false);
    blue.frame.color = {true, true};
    ASSERT_TRUE(renderer.submit(blue.frame));
    EXPECT_EQ(renderer.width(), 48);
    canvas.begin();
    renderer.draw(kTarget, kTarget);
    const auto filled = canvas.pixel(5, 5);
    EXPECT_NEAR(filled[0], 0, 6);
    EXPECT_NEAR(filled[1], 0, 6);
    EXPECT_NEAR(filled[2], 255, 6);

    renderer.clear();
    EXPECT_FALSE(renderer.hasPicture());
    canvas.begin();
    renderer.draw(kTarget, kTarget);
    EXPECT_NEAR(canvas.pixel(32, 32)[2], 128, 2);

    // Pictures the renderer cannot take are refused, not drawn half.
    media::VideoFrame empty;
    EXPECT_FALSE(renderer.submit(empty));
    media::VideoFrame onCard;
    onCard.width = 16;
    onCard.height = 16;
    onCard.layout = media::PixelLayout::D3D11Nv12; // without a texture
    EXPECT_FALSE(renderer.submit(onCard));
}

TEST(WasapiOutput, OpensTheDefaultDeviceOrSaysThereIsNone) {
    // Build machines often have no sound device. Either outcome is fine; what
    // must hold is that start, write and stop never hang or crash.
    audio::WasapiOutput output;
    EXPECT_FALSE(output.running());
    ASSERT_TRUE(output.start(48000, 2, std::chrono::milliseconds{40})); // only starts the attempt
    const bool opened = output.waitUntilOpened(std::chrono::milliseconds{5000});
    EXPECT_EQ(output.running(), opened);
    std::vector<std::int16_t> silence(480 * 2, 0);
    for (int i = 0; i < 10; ++i) output.write(silence);
    output.stop();
    EXPECT_FALSE(output.running());
    output.stop(); // twice is harmless

    // A device that does not exist is reported as missing.
    audio::WasapiOutput missing({L"no device carries this name 7f3a"});
    EXPECT_TRUE(missing.start(48000, 4, std::chrono::milliseconds{20}));
    EXPECT_FALSE(missing.waitUntilOpened(std::chrono::milliseconds{5000}));
    EXPECT_FALSE(missing.running());
}

// ---- Frame generation and scaling -------------------------------------------

namespace {

/// A render target of any size that can be read back whole.
struct Surface {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11Texture2D> staging;
    UINT width = 0;
    UINT height = 0;
    UINT vendor = 0;

    /// The mean of what was drawn.
    double mean() {
        const std::vector<int> pixels = read();
        double sum = 0.0;
        for (const int value : pixels) sum += value;
        return sum / static_cast<double>(pixels.size());
    }

    /// With `hardware`, the device is the PC's real graphics card. That
    /// fails, or yields Microsoft's software stand-in, on machines without one.
    bool create(UINT w, UINT h, bool hardware = false) {
        width = w;
        height = h;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                     levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                                     context.GetAddressOf()))) {
            return false;
        }
        if (hardware) {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC description{};
            if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
                FAILED(adapter->GetDesc(&description))) {
                return false;
            }
            vendor = description.VendorId;
            if (vendor == 0x1414) return false; // Microsoft Basic Render Driver: no card in this machine
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf()))) return false;
        if (FAILED(device->CreateRenderTargetView(target.Get(), nullptr, view.GetAddressOf()))) return false;
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()));
    }

    void begin() {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ID3D11RenderTargetView* targets[1] = {view.Get()};
        context->OMSetRenderTargets(1, targets, nullptr);
        context->ClearRenderTargetView(view.Get(), black);
    }

    /// The red channel of every pixel, row by row. The test pictures are grey.
    std::vector<int> read() {
        std::vector<int> out(static_cast<std::size_t>(width) * height, -1);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;
        for (UINT y = 0; y < height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x) out[static_cast<std::size_t>(y) * width + x] = row[x * 4];
        }
        context->Unmap(staging.Get(), 0);
        return out;
    }

    /// Blue minus red of every pixel. Brightness cancels out of that, and
    /// with it the grain of a picture: what is left is colour alone.
    std::vector<int> readTint() {
        std::vector<int> out(static_cast<std::size_t>(width) * height, 0);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;
        for (UINT y = 0; y < height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x) {
                out[static_cast<std::size_t>(y) * width + x] = static_cast<int>(row[x * 4 + 2]) - static_cast<int>(row[x * 4]);
            }
        }
        context->Unmap(staging.Get(), 0);
        return out;
    }
};

double cell(int x, int y, int seed) {
    std::uint32_t n = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u +
                      static_cast<std::uint32_t>(seed) * 982451653u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return static_cast<double>((n ^ (n >> 16)) & 0xffffu) / 65535.0;
}

/// Smooth random structure: coarse blobs with finer ones on top, nothing that repeats.
double noise(double x, double y, double size, int seed) {
    const double gx = x / size;
    const double gy = y / size;
    const int ix = static_cast<int>(std::floor(gx));
    const int iy = static_cast<int>(std::floor(gy));
    const double fx = gx - ix;
    const double fy = gy - iy;
    return (cell(ix, iy, seed) * (1 - fx) + cell(ix + 1, iy, seed) * fx) * (1 - fy) +
           (cell(ix, iy + 1, seed) * (1 - fx) + cell(ix + 1, iy + 1, seed) * fx) * fy;
}

/// A grey picture of that structure, moved by (dx, dy) pixels.
struct PatternFrame {
    std::vector<std::uint8_t> luma, u, v;
    media::VideoFrame frame;

    PatternFrame(int width, int height, double dx, double dy) {
        luma.resize(static_cast<std::size_t>(width * height));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double px = x + 0.5 - dx + 1000.0;
                const double py = y + 0.5 - dy + 1000.0;
                const double value = 0.15 + 0.7 * (0.6 * noise(px, py, 24.0, 1) + 0.4 * noise(px, py, 7.0, 2));
                luma[static_cast<std::size_t>(y * width + x)] = static_cast<std::uint8_t>(std::lround(16.0 + 219.0 * value));
            }
        }
        const int cw = (width + 1) / 2;
        const int ch = (height + 1) / 2;
        u.assign(static_cast<std::size_t>(cw * ch), 128);
        v.assign(static_cast<std::size_t>(cw * ch), 128);
        frame.width = width;
        frame.height = height;
        frame.layout = media::PixelLayout::I420;
        frame.planes[0] = luma.data();
        frame.planes[1] = u.data();
        frame.planes[2] = v.data();
        frame.strides[0] = width;
        frame.strides[1] = cw;
        frame.strides[2] = cw;
    }
};

/// Mean difference in grey levels, leaving out a border where the moved picture has no counterpart.
double meanDifference(const std::vector<int>& a, const std::vector<int>& b, int width, int height, int border) {
    double sum = 0.0;
    int count = 0;
    for (int y = border; y < height - border; ++y) {
        for (int x = border * 2; x < width - border * 2; ++x) {
            sum += std::abs(a[static_cast<std::size_t>(y * width + x)] - b[static_cast<std::size_t>(y * width + x)]);
            ++count;
        }
    }
    return sum / count;
}

} // namespace

TEST(FrameGeneration, PutsMovingContentWhereItBelongsInBetween) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    Surface surface;
    ASSERT_TRUE(surface.create(kWidth, kHeight));
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    // This is where a shader that does not compile shows up.
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    EXPECT_STREQ(renderer.frameGeneratorName(), "VeloPlay Flow");

    struct Case {
        double dx, dy;
        int step, steps;
    };
    // A slow pan, a fast one, and the three pictures between two at four times the rate.
    for (const Case& test : {Case{8, -4, 1, 2}, Case{20, 6, 1, 2}, Case{40, -10, 1, 2}, Case{12, 0, 1, 4},
                             Case{12, 0, 3, 4}, Case{0, 0, 1, 2}}) {
        SCOPED_TRACE("moved by " + std::to_string(test.dx) + ", " + std::to_string(test.dy) + ", step " +
                     std::to_string(test.step) + " of " + std::to_string(test.steps));
        const double phase = static_cast<double>(test.step) / test.steps;
        renderer.clear();

        // What the picture in between has to look like: the same content, moved part of the way.
        PatternFrame expected(kWidth, kHeight, test.dx * phase, test.dy * phase);
        ASSERT_TRUE(renderer.submit(expected.frame));
        surface.begin();
        renderer.draw(kWidth, kHeight);
        const std::vector<int> wanted = surface.read();
        renderer.clear();

        PatternFrame first(kWidth, kHeight, 0, 0);
        PatternFrame second(kWidth, kHeight, test.dx, test.dy);
        ASSERT_TRUE(renderer.submit(first.frame));
        const std::uint64_t firstId = renderer.newestId();
        ASSERT_TRUE(renderer.submit(second.frame));
        const std::uint64_t secondId = renderer.newestId();
        ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();

        render::VideoRenderer::Output output;
        output.choice.valid = true;
        output.choice.base = firstId;
        surface.begin();
        renderer.draw(kWidth, kHeight, output);
        EXPECT_FALSE(renderer.lastDrawWasGenerated());
        const std::vector<int> a = surface.read();

        output.choice.base = secondId;
        surface.begin();
        renderer.draw(kWidth, kHeight, output);
        const std::vector<int> b = surface.read();

        output.choice.base = firstId;
        output.choice.next = secondId;
        output.choice.step = test.step;
        output.choice.steps = test.steps;
        surface.begin();
        renderer.draw(kWidth, kHeight, output);
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        const std::vector<int> between = surface.read();

        // The naive answer, a mix of the two pictures, for comparison.
        std::vector<int> mixed(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            mixed[i] = static_cast<int>(std::lround(a[i] * (1.0 - phase) + b[i] * phase));
        }
        const double error = meanDifference(between, wanted, kWidth, kHeight, 24);
        const double mixError = meanDifference(mixed, wanted, kWidth, kHeight, 24);
        std::printf("frame generation: moved %.0f,%.0f step %d/%d: off by %.2f grey levels, a plain mix by %.2f\n",
                    test.dx, test.dy, test.step, test.steps, error, mixError);
        EXPECT_LT(error, 2.5);
        if (test.dx != 0 || test.dy != 0) EXPECT_LT(error, mixError * 0.4);
    }
}

TEST(VideoRenderer, TellsARepeatedPictureFromANewOne) {
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;

    // A game at 30 fps in a 60 fps stream: every picture comes twice. The
    // second time it went through the encoder again and is a level off here and there.
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    std::map<std::uint64_t, double> shares; // by picture, counted from 1
    std::map<std::uint64_t, double> means;
    for (int i = 0; i < 12; ++i) {
        PatternFrame picture(kWidth, kHeight, 6.0 * (i / 2), 2.0 * (i / 2));
        if (i % 2 == 1) {
            for (std::size_t at = 0; at < picture.luma.size(); at += 7) {
                picture.luma[at] = static_cast<std::uint8_t>(picture.luma[at] + (at % 2 == 0 ? 1 : -1));
            }
        }
        // The repeats are only measured, not kept: the picture shown stays the one before.
        const std::uint64_t shownBefore = renderer.newestId();
        ASSERT_TRUE(renderer.submit(picture.frame, i % 2 == 0));
        EXPECT_EQ(renderer.submitted(), static_cast<std::uint64_t>(i + 1));
        if (i % 2 == 1) EXPECT_EQ(renderer.newestId(), shownBefore);
        canvas.context->Flush();
        while (const auto change = renderer.takeChange()) {
            shares[change->picture] = change->share;
            means[change->picture] = change->mean;
        }
    }
    // The comparisons come a few pictures late; most of them are there by now.
    ASSERT_GE(shares.size(), 6u);
    double repeated = 0.0;
    double fresh = 255.0;
    for (const auto& [picture, share] : shares) {
        // Pictures 2, 4, 6, ... repeat the one before; 3, 5, 7, ... are new.
        if (picture % 2 == 0) {
            EXPECT_LT(share, 0.004) << "picture " << picture;
            EXPECT_LT(means[picture], 1.0) << "picture " << picture;
            repeated = std::max(repeated, means[picture]);
        } else {
            EXPECT_GT(share, 0.2) << "picture " << picture;
            EXPECT_GT(means[picture], 3.0) << "picture " << picture;
            fresh = std::min(fresh, means[picture]);
        }
    }

    // Fed to the detector, that is a game at half the stream's rate.
    media::CadenceDetector detector;
    for (std::uint64_t picture = 2; picture < 26; ++picture) detector.add(picture, picture % 2 == 0 ? repeated : fresh);
    EXPECT_TRUE(detector.halved());
    EXPECT_TRUE(detector.isRepeat(100));
}

TEST(VideoRenderer, ShowsThePictureThatIsAskedFor) {
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;

    SolidFrame dark(64, 64, 60, 128, 128, false);
    SolidFrame light(64, 64, 200, 128, 128, false);
    ASSERT_TRUE(renderer.submit(dark.frame));
    const std::uint64_t darkId = renderer.newestId();
    ASSERT_TRUE(renderer.submit(light.frame));
    const std::uint64_t lightId = renderer.newestId();
    EXPECT_GT(lightId, darkId);

    render::VideoRenderer::Output output;
    canvas.begin();
    renderer.draw(kTarget, kTarget, output); // nothing asked for: the newest
    const int lightGrey = canvas.pixel(32, 32)[0];
    output.choice.valid = true;
    output.choice.base = darkId;
    canvas.begin();
    renderer.draw(kTarget, kTarget, output);
    const int darkGrey = canvas.pixel(32, 32)[0];
    EXPECT_GT(lightGrey, darkGrey + 100);

    // Halfway between two flat pictures is the grey in the middle.
    output.choice.next = lightId;
    output.choice.step = 1;
    output.choice.steps = 2;
    canvas.begin();
    renderer.draw(kTarget, kTarget, output);
    if (renderer.frameGenerationAvailable()) {
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        EXPECT_NEAR(canvas.pixel(32, 32)[0], (lightGrey + darkGrey) / 2, 3);
    }

    // A picture that is gone cannot be shown; the newest one is.
    output.choice = {};
    output.choice.valid = true;
    output.choice.base = 12345;
    canvas.begin();
    renderer.draw(kTarget, kTarget, output);
    EXPECT_NEAR(canvas.pixel(32, 32)[0], lightGrey, 2);
}

TEST(VideoRenderer, EnlargesCleanlyAndSharpensEdges) {
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;

    // Left half dark, right half light, shown at twice the size.
    SolidFrame picture(32, 32, 80, 128, 128, false);
    for (int y = 0; y < 32; ++y) {
        for (int x = 16; x < 32; ++x) picture.luma[static_cast<std::size_t>(y * 32 + x)] = 170;
    }
    ASSERT_TRUE(renderer.submit(picture.frame));

    const auto row = [&](bool upscale, float sharpness) {
        render::VideoRenderer::Output output;
        output.upscale = upscale;
        output.sharpness = sharpness;
        canvas.begin();
        renderer.draw(kTarget, kTarget, output);
        std::array<int, kTarget> values{};
        for (UINT x = 0; x < kTarget; ++x) values[x] = canvas.pixel(x, 32)[0];
        return values;
    };
    const auto soft = row(false, 0.0f);
    const auto lanczos = row(true, 0.0f);
    const auto sharpened = row(true, 1.0f);
    const int dark = soft[8];
    const int light = soft[56];
    EXPECT_GT(light, dark + 80);

    // Flat areas stay what they are, whatever the method.
    for (const auto& values : {lanczos, sharpened}) {
        EXPECT_NEAR(values[8], dark, 2);
        EXPECT_NEAR(values[56], light, 2);
    }
    // Lanczos never leaves the range of the picture: no bright or dark echo next to the edge.
    for (UINT x = 0; x < kTarget; ++x) {
        EXPECT_GE(lanczos[x], dark - 2) << "at " << x;
        EXPECT_LE(lanczos[x], light + 2) << "at " << x;
    }
    // The step across the edge gets steeper from soft to Lanczos to sharpened.
    const auto steepest = [](const std::array<int, kTarget>& values) {
        int best = 0;
        for (UINT x = 24; x < 40; ++x) best = std::max(best, values[x + 1] - values[x]);
        return best;
    };
    std::printf("edge step: soft %d, Lanczos %d, sharpened %d\n", steepest(soft), steepest(lanczos), steepest(sharpened));
    EXPECT_GE(steepest(lanczos), steepest(soft));
    EXPECT_GT(steepest(sharpened), steepest(lanczos));
}

// ---- On the real graphics card ------------------------------------------------
//
// These run where there is one: on the self-hosted runner. On GitHub's
// machines they skip. They are the only automatic check of what the card's
// own driver does with the shaders and with NVIDIA's scaler.

TEST(OnTheGraphicsCard, GeneratesFramesLikeTheSoftwareRenderer) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    Surface surface;
    if (!surface.create(kWidth, kHeight, true)) GTEST_SKIP() << "no graphics card in this machine";
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();

    PatternFrame expected(kWidth, kHeight, 10, -3);
    ASSERT_TRUE(renderer.submit(expected.frame));
    surface.begin();
    renderer.draw(kWidth, kHeight);
    const std::vector<int> wanted = surface.read();
    renderer.clear();

    PatternFrame first(kWidth, kHeight, 0, 0);
    PatternFrame second(kWidth, kHeight, 20, -6);
    ASSERT_TRUE(renderer.submit(first.frame));
    const std::uint64_t firstId = renderer.newestId();
    ASSERT_TRUE(renderer.submit(second.frame));
    render::VideoRenderer::Output output;
    output.choice.valid = true;
    output.choice.base = firstId;
    output.choice.next = renderer.newestId();
    output.choice.step = 1;
    output.choice.steps = 2;
    surface.begin();
    renderer.draw(kWidth, kHeight, output);
    EXPECT_TRUE(renderer.lastDrawWasGenerated());
    const double off = meanDifference(surface.read(), wanted, kWidth, kHeight, 24);
    std::printf("on the graphics card (vendor 0x%04x): generated frame off by %.2f grey levels\n", surface.vendor, off);
    EXPECT_LT(off, 2.5);
}

/// A ladder in front of a background, both moving with the camera: bright
/// rungs sixteen pixels apart. Moved by nearly a rung's distance, every rung
/// fits the next one just as well, which is where a search from coarse to
/// fine goes wrong.
struct LadderFrame {
    PatternFrame picture;

    LadderFrame(int width, int height, double dx, double dy) : picture(width, height, dx, dy) {
        const double left = width * 0.35 + dx;
        const double right = width * 0.65 + dx;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (x + 0.5 < left || x + 0.5 >= right) continue;
                const double along = std::fmod(y + 0.5 - dy + 1600.0, 16.0);
                const bool rail = x + 0.5 < left + 6.0 || x + 0.5 >= right - 6.0;
                const double value = (rail || along < 5.0) ? 0.85 : 0.12;
                picture.luma[static_cast<std::size_t>(y * width + x)] =
                    static_cast<std::uint8_t>(std::lround(16.0 + 219.0 * value));
            }
        }
    }
};

/// Something blue in front of grey that is exactly as bright. That is what
/// the nozzle at the petrol station was: in brightness there is nothing to
/// see but the grain the encoder leaves, different in every picture; the
/// shape shows in colour alone. With `hand`, a patch with structure moves
/// along, as the hand that held the nozzle did.
struct TintedShapeFrame {
    PatternFrame picture;

    static bool inside(double x, double y, int width, int height) {
        const double ex = (x - width * 0.56) / (width * 0.17);
        const double ey = (y - height * 0.42) / (height * 0.27);
        if (ex * ex + ey * ey < 1.0) return true;
        // The grip: a bar that runs down to the left.
        const double along = (x - width * 0.30) * 0.8 - (y - height * 0.80) * 0.6;
        const double across = (x - width * 0.30) * 0.6 + (y - height * 0.80) * 0.8;
        return along > 0.0 && along < width * 0.30 && std::abs(across) < height * 0.07;
    }

    TintedShapeFrame(int width, int height, double dx, double dy, int grain, bool hand)
        : picture(width, height, dx, dy) {
        const double handX = width * 0.36 + dx;
        const double handY = height * 0.58 + dy;
        const double handRadius = height * 0.13;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                auto& value = picture.luma[static_cast<std::size_t>(y * width + x)];
                const double hx = x + 0.5 - handX;
                const double hy = y + 0.5 - handY;
                if (hand && hx * hx + hy * hy < handRadius * handRadius) continue; // the moved structure stays
                value = static_cast<std::uint8_t>(std::lround(78.0 + 5.0 * (noise(x, y, 5.0, grain) - 0.5) +
                                                               2.0 * (cell(x, y, grain + 50) - 0.5)));
            }
        }
        const int cw = (width + 1) / 2;
        const int ch = (height + 1) / 2;
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                // How much of the four pixels this colour sample stands for lies in the shape.
                double covered = 0.0;
                for (int j = 0; j < 2; ++j) {
                    for (int i = 0; i < 2; ++i) {
                        if (inside(x * 2 + i + 0.5 - dx, y * 2 + j + 0.5 - dy, width, height)) covered += 0.25;
                    }
                }
                picture.u[static_cast<std::size_t>(y * cw + x)] = static_cast<std::uint8_t>(std::lround(128.0 + 20.0 * covered));
                picture.v[static_cast<std::size_t>(y * cw + x)] = static_cast<std::uint8_t>(std::lround(128.0 - 17.0 * covered));
            }
        }
    }
};

/// How far the picture computed between `first` and `second` is from `expected`, in grey levels,
/// and how long computing one takes. With `card` false the built-in motion search is used.
/// `tintError` and `tintMixError` are the same for colour alone (blue minus red), which no grain touches.
struct Generated {
    double error = 0.0;
    double mixError = 0.0;
    double tintError = 0.0;
    double tintMixError = 0.0;
    double milliseconds = 0.0;
};
Generated generateBetween(Surface& surface, render::VideoRenderer& renderer, const media::VideoFrame& first,
                          const media::VideoFrame& second, const media::VideoFrame& expected, int width, int height,
                          int rounds = 12) {
    renderer.clear();
    EXPECT_TRUE(renderer.submit(expected));
    surface.begin();
    renderer.draw(static_cast<UINT>(width), static_cast<UINT>(height));
    const std::vector<int> wanted = surface.read();
    const std::vector<int> wantedTint = surface.readTint();

    render::VideoRenderer::Output output;
    output.choice.valid = true;
    const auto show = [&](const media::VideoFrame& frame) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(frame));
        surface.begin();
        renderer.draw(static_cast<UINT>(width), static_cast<UINT>(height));
        return surface.read();
    };
    const std::vector<int> a = show(first);
    const std::vector<int> aTint = surface.readTint();
    const std::vector<int> b = show(second);
    const std::vector<int> bTint = surface.readTint();

    Generated result;
    std::vector<int> between;
    // Several rounds: the first one includes setting things up.
    const int kRounds = std::max(1, rounds);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    double best = 1e9;
    for (int round = 0; round < kRounds; ++round) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(first));
        const std::uint64_t firstId = renderer.newestId();
        surface.begin();
        renderer.draw(static_cast<UINT>(width), static_cast<UINT>(height));
        (void)surface.read(); // the card is done with everything before the clock starts
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        EXPECT_TRUE(renderer.submit(second));
        output.choice.base = firstId;
        output.choice.next = renderer.newestId();
        output.choice.step = 1;
        output.choice.steps = 2;
        surface.begin();
        renderer.draw(static_cast<UINT>(width), static_cast<UINT>(height), output);
        between = surface.read(); // waits for the card
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        best = std::min(best, static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
    }
    const std::vector<int> betweenTint = surface.readTint();
    std::vector<int> mixed(a.size());
    std::vector<int> mixedTint(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        mixed[i] = (a[i] + b[i] + 1) / 2;
        mixedTint[i] = (aTint[i] + bTint[i]) / 2;
    }
    result.error = meanDifference(between, wanted, width, height, 24);
    result.mixError = meanDifference(mixed, wanted, width, height, 24);
    result.tintError = meanDifference(betweenTint, wantedTint, width, height, 24);
    result.tintMixError = meanDifference(mixedTint, wantedTint, width, height, 24);
    result.milliseconds = best;
    return result;
}

// The nozzle at the petrol station (a user's report): blue in front of grey
// of the same brightness flickered badly in computed pictures.
TEST(FrameGeneration, MovesColourThatBrightnessDoesNotShow) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    Surface surface;
    ASSERT_TRUE(surface.create(kWidth, kHeight));
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    renderer.holdStillParts(false);
    for (const bool hand : {false, true}) {
        TintedShapeFrame first(kWidth, kHeight, 0.0, 0.0, 11, hand);
        TintedShapeFrame second(kWidth, kHeight, 8.0, 4.0, 12, hand);
        TintedShapeFrame expected(kWidth, kHeight, 4.0, 2.0, 13, hand);
        const Generated own = generateBetween(surface, renderer, first.picture.frame, second.picture.frame,
                                              expected.picture.frame, kWidth, kHeight, 1);
        std::printf("blue shape as bright as the grey behind it%s: colour off by %.3f, by %.3f in a plain mix\n",
                    hand ? ", held by a hand" : "", own.tintError, own.tintMixError);
        // Before the search compared colour, this was no better than the plain mix.
        EXPECT_LT(own.tintError, own.tintMixError * 0.6) << (hand ? "with hand" : "alone");
    }
}

TEST(FrameGeneration, TheShadersForTheCardsMotionSearchCompile) {
    // No NVIDIA card here, so the card's search is not used. The reason has
    // to be the card and not a shader that did not compile: the shaders are
    // built on every machine for exactly this check.
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    EXPECT_FALSE(renderer.usesCardMotion());
    EXPECT_EQ(renderer.cardMotionError(), "not an NVIDIA graphics card");
    EXPECT_STREQ(renderer.frameGeneratorName(), "VeloPlay Flow");
}

TEST(FrameGeneration, ShowsARealPictureAcrossACut) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    Surface surface;
    ASSERT_TRUE(surface.create(kWidth, kHeight));
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;

    // The game jumps to another scene: the second picture is a different place of the structure altogether.
    PatternFrame first(kWidth, kHeight, 0, 0);
    PatternFrame second(kWidth, kHeight, 5003, 2999);
    const auto show = [&](const media::VideoFrame& frame) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(frame));
        surface.begin();
        renderer.draw(kWidth, kHeight);
        return surface.read();
    };
    const std::vector<int> a = show(first.frame);
    const std::vector<int> b = show(second.frame);
    std::vector<int> mixed(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) mixed[i] = (a[i] + b[i] + 1) / 2;

    renderer.clear();
    ASSERT_TRUE(renderer.submit(first.frame));
    const std::uint64_t firstId = renderer.newestId();
    ASSERT_TRUE(renderer.submit(second.frame));
    const auto between = [&](int step, int steps) {
        render::VideoRenderer::Output output;
        output.choice.valid = true;
        output.choice.base = firstId;
        output.choice.next = renderer.newestId();
        output.choice.step = step;
        output.choice.steps = steps;
        surface.begin();
        renderer.draw(kWidth, kHeight, output);
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        return surface.read();
    };
    // A mix of two scenes shows as a flash of both at once. Shown is the nearer real picture.
    const std::vector<int> early = between(1, 4);
    const std::vector<int> late = between(3, 4);
    const double earlyFromFirst = meanDifference(early, a, kWidth, kHeight, 0);
    const double lateFromSecond = meanDifference(late, b, kWidth, kHeight, 0);
    const double apart = meanDifference(a, b, kWidth, kHeight, 0);
    std::printf("cut: the two scenes are %.1f grey levels apart; a quarter in is %.2f from the first, three quarters "
                "in %.2f from the second\n",
                apart, earlyFromFirst, lateFromSecond);
    EXPECT_GT(apart, 20.0);
    EXPECT_LT(earlyFromFirst, 1.0);
    EXPECT_LT(lateFromSecond, 1.0);
}

/// A scene that moves behind something that does not: the game's own display,
/// here a block of fine upright bars like the strokes of writing.
struct OverlayFrame {
    static constexpr int kLeft = 60, kTop = 30, kRight = 150, kBottom = 62;
    PatternFrame picture;

    OverlayFrame(int width, int height, double dx, double dy) : picture(width, height, dx, dy) {
        for (int y = kTop; y < kBottom; ++y) {
            for (int x = kLeft; x < kRight; ++x) {
                const bool stroke = (x - kLeft) % 6 < 3;
                picture.luma[static_cast<std::size_t>(y * width + x)] = static_cast<std::uint8_t>(stroke ? 220 : 40);
            }
        }
    }
};

namespace {

/// A display of fine strokes stands over a scene that moves, for `kPictures`
/// pictures. In the picture computed between the last two it has to be where
/// it is, and the scene around it must not pay for that.
void checkStandingDisplay(const int kPictures) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    constexpr double kStepX = 4.0;
    constexpr double kStepY = 1.0;
    Surface surface;
    ASSERT_TRUE(surface.create(kWidth, kHeight));

    // The picture between the last two, as it should be: the scene half a step on, the display unmoved.
    std::vector<int> wanted;
    {
        render::VideoRenderer reference;
        std::string error;
        ASSERT_TRUE(reference.create(surface.device.Get(), surface.context.Get(), error)) << error;
        OverlayFrame expected(kWidth, kHeight, kStepX * (kPictures - 1.5), kStepY * (kPictures - 1.5));
        ASSERT_TRUE(reference.submit(expected.picture.frame));
        surface.begin();
        reference.draw(kWidth, kHeight);
        wanted = surface.read();
    }
    const auto offBy = [&](const std::vector<int>& picture, bool inside) {
        double sum = 0.0;
        int count = 0;
        for (int y = 24; y < kHeight - 24; ++y) {
            for (int x = 48; x < kWidth - 48; ++x) {
                const bool display = x >= OverlayFrame::kLeft - 2 && x < OverlayFrame::kRight + 2 &&
                                     y >= OverlayFrame::kTop - 2 && y < OverlayFrame::kBottom + 2;
                if (display != inside) continue;
                sum += std::abs(picture[static_cast<std::size_t>(y * kWidth + x)] - wanted[static_cast<std::size_t>(y * kWidth + x)]);
                ++count;
            }
        }
        return sum / count;
    };

    double displayError[2] = {0.0, 0.0};
    double sceneError[2] = {0.0, 0.0};
    for (int hold = 0; hold < 2; ++hold) {
        render::VideoRenderer renderer;
        std::string error;
        ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
        renderer.holdStillParts(hold == 1);
        std::uint64_t before = 0;
        std::vector<int> between;
        for (int i = 0; i < kPictures; ++i) {
            OverlayFrame picture(kWidth, kHeight, kStepX * i, kStepY * i);
            ASSERT_TRUE(renderer.submit(picture.picture.frame));
            if (i > 0) {
                render::VideoRenderer::Output output;
                output.choice.valid = true;
                output.choice.base = before;
                output.choice.next = renderer.newestId();
                output.choice.step = 1;
                output.choice.steps = 2;
                surface.begin();
                renderer.draw(kWidth, kHeight, output);
                ASSERT_TRUE(renderer.lastDrawWasGenerated());
                if (i == kPictures - 1) between = surface.read();
            }
            before = renderer.newestId();
        }
        displayError[hold] = offBy(between, true);
        sceneError[hold] = offBy(between, false);
    }
    std::printf("standing display over a moving scene, %d pictures: display off by %.2f grey levels without care and "
                "%.2f with it; scene off by %.2f and %.2f\n",
                kPictures, displayError[0], displayError[1], sceneError[0], sceneError[1]);
    // The display is where it belongs, and caring for it did not cost the scene anything.
    EXPECT_LT(displayError[1], 1.5);
    EXPECT_LE(displayError[1], displayError[0] + 0.1);
    EXPECT_LT(sceneError[1], sceneError[0] + 0.3);
    EXPECT_LT(sceneError[1], 2.5);
}

} // namespace

TEST(FrameGeneration, KeepsWhatStandsStillWhereItIs) {
    checkStandingDisplay(44); // a second and a half at 30 pictures a second
}

// A button the game put up half a second ago. The count for standing parts
// used to need about a second, and that long the button was dragged along
// with the scene behind it, which showed as a flicker.
TEST(FrameGeneration, KeepsWhatAppearedHalfASecondAgoWhereItIs) {
    checkStandingDisplay(16);
}

// The other side of holding what stands still: what keeps moving must never
// be taken for standing. A ladder scrolls past for most of a second; the
// gaps between its rungs look the same in many a pair of pictures, and still
// nothing there may be held. The picture between the last two has to come
// out the same with the care switched on as with it switched off.
TEST(FrameGeneration, DoesNotHoldALadderThatKeepsMoving) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    constexpr int kPictures = 24;
    constexpr double kStepY = 6.0; // rungs are 16 apart: every gap is crossed by a rung within three pictures
    Surface surface;
    ASSERT_TRUE(surface.create(kWidth, kHeight));

    std::vector<int> between[2];
    for (int hold = 0; hold < 2; ++hold) {
        render::VideoRenderer renderer;
        std::string error;
        ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
        renderer.holdStillParts(hold == 1);
        std::uint64_t before = 0;
        for (int i = 0; i < kPictures; ++i) {
            LadderFrame picture(kWidth, kHeight, 0.0, kStepY * i);
            ASSERT_TRUE(renderer.submit(picture.picture.frame));
            if (i > 0) {
                render::VideoRenderer::Output output;
                output.choice.valid = true;
                output.choice.base = before;
                output.choice.next = renderer.newestId();
                output.choice.step = 1;
                output.choice.steps = 2;
                surface.begin();
                renderer.draw(kWidth, kHeight, output);
                ASSERT_TRUE(renderer.lastDrawWasGenerated());
                if (i == kPictures - 1) between[hold] = surface.read();
            }
            before = renderer.newestId();
        }
    }
    // The rails do stand still in this picture, and they are held together
    // with the one pixel next to them, as the soft rim of a letter would be.
    // That rim is where the two results may differ. The rungs and the gaps
    // between them, away from the rails, and the scene around the ladder
    // have to come out the same.
    const int left = static_cast<int>(kWidth * 0.35);
    const int right = static_cast<int>(kWidth * 0.65);
    double rungs = 0.0, rims = 0.0, scene = 0.0;
    int rungCount = 0, rimCount = 0, sceneCount = 0;
    for (int y = 8; y < kHeight - 8; ++y) {
        for (int x = 16; x < kWidth - 16; ++x) {
            const std::size_t at = static_cast<std::size_t>(y * kWidth + x);
            const double difference = std::abs(between[1][at] - between[0][at]);
            if (x >= left + 9 && x < right - 9) {
                rungs += difference;
                ++rungCount;
            } else if (x >= left - 3 && x < right + 3) {
                rims += difference;
                ++rimCount;
            } else {
                scene += difference;
                ++sceneCount;
            }
        }
    }
    rungs /= rungCount;
    rims /= rimCount;
    scene /= sceneCount;
    std::printf("ladder that keeps moving, with and without care for standing parts: rungs %.2f grey levels apart, "
                "scene %.2f, rails and their rims %.2f\n",
                rungs, scene, rims);
    EXPECT_LT(rungs, 0.5);
    EXPECT_LT(scene, 0.5);
}

TEST(OnTheGraphicsCard, FindsMotionWithNvidiasEngineWhereThereIsOne) {
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    Surface surface;
    if (!surface.create(kWidth, kHeight, true)) GTEST_SKIP() << "no graphics card in this machine";
    if (surface.vendor != 0x10DE) GTEST_SKIP() << "not an NVIDIA graphics card";
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    if (!renderer.usesCardMotion()) {
        // An NVIDIA card without the engine (older than the RTX 20 series) or without its library.
        GTEST_SKIP() << "the card's motion search is not available: " << renderer.cardMotionError();
    }

    struct Scene {
        const char* name;
        bool ladder;
        double dx, dy;
    };
    const Scene scenes[] = {
        {"textured picture, slow", false, 6, -2},
        {"textured picture, fast", false, 26, -8},
        {"ladder, camera moving down", true, 2, 14},
        {"ladder, camera moving sideways", true, 18, 0},
    };
    // This test measures the search for motion. It hands the same pair of
    // pictures in again and again, for the timing, and to the care for parts
    // that stand still that looks like a picture that never changes: the gaps
    // between the rungs get held. In a game the pairs follow one another and
    // nothing of the kind happens (see DoesNotHoldALadderThatKeepsMoving).
    renderer.holdStillParts(false);
    for (const Scene& scene : scenes) {
        const auto frame = [&](double share) {
            return scene.ladder ? LadderFrame(kWidth, kHeight, scene.dx * share, scene.dy * share).picture
                                : PatternFrame(kWidth, kHeight, scene.dx * share, scene.dy * share);
        };
        PatternFrame first = frame(0.0);
        PatternFrame second = frame(1.0);
        PatternFrame expected = frame(0.5);
        // PatternFrame holds pointers into itself; after the copies above they have to be set again.
        for (PatternFrame* p : {&first, &second, &expected}) {
            p->frame.planes[0] = p->luma.data();
            p->frame.planes[1] = p->u.data();
            p->frame.planes[2] = p->v.data();
        }

        renderer.allowCardMotion(true);
        const Generated card = generateBetween(surface, renderer, first.frame, second.frame, expected.frame, kWidth, kHeight);
        // The engine has to have done it, not the fallback after a failure.
        ASSERT_TRUE(renderer.usesCardMotion()) << renderer.cardMotionError();
        renderer.allowCardMotion(false);
        const Generated own = generateBetween(surface, renderer, first.frame, second.frame, expected.frame, kWidth, kHeight);
        std::printf("motion search, %s: NVIDIA off by %.2f grey levels in %.2f ms, built-in off by %.2f in %.2f ms, "
                    "a plain mix off by %.2f\n",
                    scene.name, card.error, card.milliseconds, own.error, own.milliseconds, card.mixError);
        // It has to be right, and not clearly worse than what it replaces.
        EXPECT_LT(card.error, card.mixError * 0.6) << scene.name;
        EXPECT_LT(card.error, own.error * 1.25 + 0.5) << scene.name;
    }

    // Colour that brightness does not show. On the RTX 4070: 2.68 and 2.17
    // while everything compared brightness alone, 0.19 and 0.18 since colour
    // counts and the built-in search may help, against 1.21 for a plain mix.
    for (const bool hand : {false, true}) {
        TintedShapeFrame first(kWidth, kHeight, 0.0, 0.0, 11, hand);
        TintedShapeFrame second(kWidth, kHeight, 10.0, 4.0, 12, hand);
        TintedShapeFrame expected(kWidth, kHeight, 5.0, 2.0, 13, hand);
        renderer.allowCardMotion(true);
        const Generated card = generateBetween(surface, renderer, first.picture.frame, second.picture.frame,
                                               expected.picture.frame, kWidth, kHeight);
        ASSERT_TRUE(renderer.usesCardMotion()) << renderer.cardMotionError();
        renderer.allowCardMotion(false);
        const Generated own = generateBetween(surface, renderer, first.picture.frame, second.picture.frame,
                                              expected.picture.frame, kWidth, kHeight);
        // Short lines: the build prints them into a narrow window.
        std::printf("motion search, blue on grey%s: NVIDIA %.3f, built-in %.3f, plain mix %.3f\n",
                    hand ? " with hand" : "", card.tintError, own.tintError, card.tintMixError);
        EXPECT_LT(card.tintError, card.tintMixError * 0.6) << (hand ? "with hand" : "alone");
    }
}

// The other tests compute small pictures. This one says how long a picture
// takes at the size a game arrives in, with the upload and the reading back.
TEST(OnTheGraphicsCard, SaysHowLongAPictureTakesAtFullSize) {
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    Surface surface;
    if (!surface.create(kWidth, kHeight, true)) GTEST_SKIP() << "no graphics card in this machine";
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    renderer.holdStillParts(false);
    PatternFrame first(kWidth, kHeight, 0, 0);
    PatternFrame second(kWidth, kHeight, 18, -6);
    PatternFrame expected(kWidth, kHeight, 9, -3);
    const bool card = renderer.usesCardMotion();
    const Generated with = generateBetween(surface, renderer, first.frame, second.frame, expected.frame, kWidth, kHeight, 8);
    renderer.allowCardMotion(false);
    const Generated own = generateBetween(surface, renderer, first.frame, second.frame, expected.frame, kWidth, kHeight, 8);
    std::printf("motion search at 1920 x 1080: %s %.2f ms (off by %.2f), built-in alone %.2f ms (off by %.2f)\n",
                card ? "with NVIDIA" : "no card search,", with.milliseconds, with.error, own.milliseconds, own.error);
    EXPECT_LT(own.error, own.mixError * 0.6);
    EXPECT_LT(with.error, with.mixError * 0.6);
}

TEST(OnTheGraphicsCard, EnlargesWithNvidiasScalerWhereThereIsOne) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    Surface surface;
    if (!surface.create(kWidth * 2, kHeight * 2, true)) GTEST_SKIP() << "no graphics card in this machine";
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    if (!renderer.rtxAvailable()) GTEST_SKIP() << "no RTX Video on this card: " << renderer.rtxError();

    PatternFrame picture(kWidth, kHeight, 0, 0);
    ASSERT_TRUE(renderer.submit(picture.frame));
    render::VideoRenderer::Output output;
    output.upscale = true;
    surface.begin();
    renderer.draw(kWidth * 2, kHeight * 2, output);
    EXPECT_STREQ(renderer.scalerName(), "Lanczos");
    const std::vector<int> lanczos = surface.read();
    const double lanczosMean = surface.mean();

    output.rtx = true;
    surface.begin();
    renderer.draw(kWidth * 2, kHeight * 2, output);
    // The driver took the picture and gave one back ...
    ASSERT_STREQ(renderer.scalerName(), "RTX Video") << renderer.rtxError();
    const std::vector<int> rtx = surface.read();
    const double rtxMean = surface.mean();
    // ... that shows the same thing: same brightness overall, and close to the
    // Lanczos result point by point. How much better it looks no test can say.
    const double apart = meanDifference(rtx, lanczos, kWidth * 2, kHeight * 2, 16);
    std::printf("RTX Video: mean %.1f against Lanczos %.1f, %.2f grey levels apart on average\n", rtxMean, lanczosMean,
                apart);
    EXPECT_NEAR(rtxMean, lanczosMean, 4.0);
    EXPECT_LT(apart, 12.0);

    // Sharpening on top goes through as well.
    output.sharpness = 0.5f;
    surface.begin();
    renderer.draw(kWidth * 2, kHeight * 2, output);
    EXPECT_STREQ(renderer.scalerName(), "RTX Video");
    EXPECT_NEAR(surface.mean(), lanczosMean, 6.0);
}

TEST(VideoRenderer, FallsBackToLanczosWithoutNvidiasScaler) {
    Canvas canvas;
    ASSERT_TRUE(canvas.create());
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(canvas.device.Get(), canvas.context.Get(), error)) << error;
    // The software renderer is not an NVIDIA card.
    EXPECT_FALSE(renderer.rtxAvailable());
    EXPECT_FALSE(renderer.rtxError().empty());

    SolidFrame picture(32, 32, 120, 128, 128, false);
    ASSERT_TRUE(renderer.submit(picture.frame));
    render::VideoRenderer::Output output;
    output.upscale = true;
    output.rtx = true;
    canvas.begin();
    renderer.draw(kTarget, kTarget, output);
    EXPECT_STREQ(renderer.scalerName(), "Lanczos");
    const int grey = canvas.pixel(32, 32)[0];
    EXPECT_GT(grey, 100);
    EXPECT_LT(grey, 140);

    // At its own size nothing is enlarged, and nothing claims to be.
    canvas.begin();
    renderer.draw(32, 32, output);
    EXPECT_STREQ(renderer.scalerName(), "");
}

// ---- Where does a thread wait? ----------------------------------------------

TEST(ThreadStack, ShowsWhereAnotherThreadWaits) {
    diag::ThreadStack stack;
    // Nothing to look at yet, and never the calling thread itself.
    EXPECT_TRUE(stack.capture().empty());
    stack.watchCurrentThread();
    EXPECT_TRUE(stack.capture().empty());

    diag::ThreadStack sleeper;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread([&] {
        sleeper.watchCurrentThread();
        SetEvent(ready);
        WaitForSingleObject(release, 30'000);
    });
    WaitForSingleObject(ready, 30'000);
    Sleep(100); // let it reach the wait

    const std::vector<std::string> lines = sleeper.capture();
    SetEvent(release);
    thread.join();
    CloseHandle(ready);
    CloseHandle(release);

    // The wait itself is in ntdll, the call into it in kernelbase or
    // kernel32, and further out comes this test program.
    ASSERT_GE(lines.size(), 3u);
    for (const std::string& line : lines) SCOPED_TRACE(line);
    EXPECT_NE(lines.front().find("ntdll"), std::string::npos) << lines.front();
    bool sawThisProgram = false;
    bool sawFunctionName = false;
    for (const std::string& line : lines) {
        if (line.find("veloplay_tests") != std::string::npos) sawThisProgram = true;
        if (line.find('(') != std::string::npos) sawFunctionName = true;
    }
    EXPECT_TRUE(sawThisProgram);
    EXPECT_TRUE(sawFunctionName);
}

// Whether the driver applies its network cannot be asked. It can be seen:
// the same picture goes through the video processor twice, once with
// NVIDIA's extension switched on and once without it ever being touched.
// If the two results are the same, the extension does nothing on this PC,
// whatever the setting in VeloPlay says. Nothing is asserted about the
// difference, because it depends on NVIDIA's own settings; the numbers are
// printed and read from the build's log.
TEST(OnTheGraphicsCard, ShowsWhetherNvidiasNetworkDoesAnything) {
    Surface surface;
    if (!surface.create(16, 16, true)) GTEST_SKIP() << "no graphics card in this machine";
    render::RtxVideoScaler scaler;
    std::string reason;
    if (!scaler.create(surface.device.Get(), surface.context.Get(), reason)) GTEST_SKIP() << "no RTX Video on this card: " << reason;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        std::printf("RTX Video: the primary screen is %lu x %lu at %lu Hz\n", static_cast<unsigned long>(mode.dmPelsWidth),
                    static_cast<unsigned long>(mode.dmPelsHeight), static_cast<unsigned long>(mode.dmDisplayFrequency));
    }

    struct Case {
        int sourceWidth, sourceHeight, width, height;
    };
    const std::array<Case, 4> cases = {{
        {960, 540, 1920, 1080},
        {1920, 1080, 2560, 1440},
        {1920, 1080, 3840, 2160},
        {1920, 1080, 1920, 1080}, // not enlarged at all: does the driver clean the picture up anyway?
    }};
    for (const Case& test : cases) {
        // Something like a game picture after the encoder: soft structure,
        // hard edges, thin lines, and blocks of eight pixels that are a
        // level or two off.
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(test.sourceWidth) * test.sourceHeight * 4);
        for (int y = 0; y < test.sourceHeight; ++y) {
            for (int x = 0; x < test.sourceWidth; ++x) {
                double value = 0.15 + 0.6 * (0.6 * noise(x + 1000.5, y + 1000.5, 48.0, 1) + 0.4 * noise(x + 1000.5, y + 1000.5, 9.0, 2));
                if ((x / 160 + y / 120) % 2 == 0 && x % 160 > 40 && y % 120 > 30) value += 0.2;
                if ((x + y) % 97 < 2 || (x * 3 - y + 4000) % 211 < 2) value = 0.9;
                value += (cell(x / 8, y / 8, 3) - 0.5) * 0.012;
                const auto grey = static_cast<std::uint8_t>(std::lround(255.0 * std::clamp(value, 0.0, 1.0)));
                std::uint8_t* out = &pixels[(static_cast<std::size_t>(y) * test.sourceWidth + x) * 4];
                out[0] = out[1] = out[2] = grey;
                out[3] = 255;
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(test.sourceWidth);
        desc.Height = static_cast<UINT>(test.sourceHeight);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = static_cast<UINT>(test.sourceWidth) * 4;
        ComPtr<ID3D11Texture2D> source;
        ASSERT_TRUE(SUCCEEDED(surface.device->CreateTexture2D(&desc, &data, source.GetAddressOf())));

        desc.Width = static_cast<UINT>(test.width);
        desc.Height = static_cast<UINT>(test.height);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ASSERT_TRUE(SUCCEEDED(surface.device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())));

        // The result as grey levels and the time one call takes, the read-back included.
        const auto run = [&](bool network, std::vector<int>& out, double& ms) {
            scaler.useNetwork(network);
            constexpr int kRounds = 6; // the first one sets the processor up and is not timed
            double total = 0.0;
            for (int round = 0; round < kRounds; ++round) {
                const auto start = std::chrono::steady_clock::now();
                ID3D11ShaderResourceView* view =
                    scaler.scale(source.Get(), test.sourceWidth, test.sourceHeight, test.width, test.height);
                if (!view) return false;
                ComPtr<ID3D11Resource> result;
                view->GetResource(result.GetAddressOf());
                surface.context->CopyResource(staging.Get(), result.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(surface.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
                if (round == kRounds - 1) {
                    out.resize(static_cast<std::size_t>(test.width) * test.height);
                    for (int y = 0; y < test.height; ++y) {
                        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
                        for (int x = 0; x < test.width; ++x) out[static_cast<std::size_t>(y) * test.width + x] = row[x * 4];
                    }
                }
                surface.context->Unmap(staging.Get(), 0);
                if (round > 0) total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
            ms = total / (kRounds - 1);
            return true;
        };

        std::vector<int> plain, network;
        double plainMs = 0.0, networkMs = 0.0;
        if (!run(false, plain, plainMs)) {
            std::printf("RTX Video %d x %d to %d x %d: the video processor refused: %s\n", test.sourceWidth, test.sourceHeight,
                        test.width, test.height, scaler.error().c_str());
            continue;
        }
        if (!run(true, network, networkMs)) {
            std::printf("RTX Video %d x %d to %d x %d: refused with the network switched on: %s\n", test.sourceWidth,
                        test.sourceHeight, test.width, test.height, scaler.error().c_str());
            continue;
        }
        double sum = 0.0;
        int largest = 0;
        std::size_t differing = 0;
        for (std::size_t i = 0; i < plain.size(); ++i) {
            const int difference = std::abs(plain[i] - network[i]);
            sum += difference;
            largest = std::max(largest, difference);
            if (difference > 1) ++differing;
        }
        std::printf("RTX Video %d x %d to %d x %d: with the network %.2f grey levels away from the plain filter on average, "
                    "%d at most, %.1f %% of the picture differs; %.2f ms against %.2f ms\n",
                    test.sourceWidth, test.sourceHeight, test.width, test.height, sum / static_cast<double>(plain.size()),
                    largest, 100.0 * static_cast<double>(differing) / static_cast<double>(plain.size()), networkMs, plainMs);
    }
}

// The user's own case: a 1080p stream on a 1080p screen. Nothing is enlarged,
// and still the network is to get the picture, because it cleans it up.
TEST(OnTheGraphicsCard, LetsNvidiasNetworkWorkOnAPictureThatKeepsItsSize) {
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    Surface surface;
    if (!surface.create(kWidth, kHeight, true)) GTEST_SKIP() << "no graphics card in this machine";
    render::VideoRenderer renderer;
    std::string error;
    ASSERT_TRUE(renderer.create(surface.device.Get(), surface.context.Get(), error)) << error;
    if (!renderer.rtxAvailable()) GTEST_SKIP() << "no RTX Video on this card: " << renderer.rtxError();

    PatternFrame picture(kWidth, kHeight, 0, 0);
    ASSERT_TRUE(renderer.submit(picture.frame));
    render::VideoRenderer::Output output;
    output.upscale = true;
    surface.begin();
    renderer.draw(kWidth, kHeight, output);
    // Lanczos has nothing to do at the same size and says so.
    EXPECT_STREQ(renderer.scalerName(), "");
    const std::vector<int> plain = surface.read();
    const double plainMean = surface.mean();

    output.rtx = true;
    surface.begin();
    renderer.draw(kWidth, kHeight, output);
    EXPECT_STREQ(renderer.scalerName(), "RTX Video") << renderer.rtxError();
    const std::vector<int> cleaned = surface.read();
    const double apart = meanDifference(cleaned, plain, kWidth, kHeight, 16);
    std::printf("RTX Video at the same size: mean %.1f against %.1f untouched, %.2f grey levels apart on average\n",
                surface.mean(), plainMean, apart);
    // The same picture, not a different one.
    EXPECT_NEAR(surface.mean(), plainMean, 4.0);
    EXPECT_LT(apart, 12.0);

    // A window smaller than the stream: the picture is shrunk by the plain filter, the network stays out of it.
    surface.begin();
    renderer.draw(kWidth / 2, kHeight / 2, output);
    EXPECT_STREQ(renderer.scalerName(), "");
}
