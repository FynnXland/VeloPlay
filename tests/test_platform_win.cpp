// Windows only. Draws with WARP, the software renderer every Windows has, so
// the shader and the upload paths are checked on machines without a graphics card.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
        while (const auto change = renderer.takeChange()) shares[change->picture] = change->share;
    }
    // The comparisons come a few pictures late; most of them are there by now.
    ASSERT_GE(shares.size(), 6u);
    double repeated = 0.0;
    double fresh = 1.0;
    for (const auto& [picture, share] : shares) {
        // Pictures 2, 4, 6, ... repeat the one before; 3, 5, 7, ... are new.
        if (picture % 2 == 0) {
            EXPECT_LT(share, media::CadenceDetector::kRepeatBelow) << "picture " << picture;
            repeated = std::max(repeated, share);
        } else {
            EXPECT_GT(share, 0.2) << "picture " << picture;
            fresh = std::min(fresh, share);
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
