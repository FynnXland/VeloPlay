// The Linux picture, drawn with OpenGL 3.3 core on whatever the machine
// offers: on the build machines that is Mesa's llvmpipe on the processor,
// run under xvfb-run. The tests follow those of the Direct3D renderer in
// test_platform_win.cpp, with the same pictures, so the numbers they print
// can be laid side by side.
//
// The context comes from GLFW, loaded at run time: the test program is not
// linked against it (tests/CMakeLists.txt is shared by all parts), and a
// machine without it or without a display skips these tests instead of
// failing to start.

#include <gtest/gtest.h>

#include <dlfcn.h>

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

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "linux/GlCapture.hpp"
#include "linux/GlFunctions.hpp"
#include "linux/GlVideoRenderer.hpp"
#include "media/FramePacer.hpp"
#include "media/Snapshot.hpp"

using namespace veloplay;
using linuxos::GlFunctions;

namespace {

// glfwGetProcAddress, kept outside the window so the loader can reach it while the window is being made.
decltype(&glfwGetProcAddress) gGetProcAddress = nullptr;

/// One hidden window with an OpenGL 3.3 core context for the whole test program.
class GlWindow {
public:
    static GlWindow& get() {
        static GlWindow window;
        return window;
    }
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] const std::string& why() const { return why_; }
    [[nodiscard]] linuxos::GlProcLoader loader() const { return &GlWindow::find; }
    const GlFunctions& gl() const { return gl_; }

private:
    GlWindow() {
        library_ = dlopen("libglfw.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!library_) {
            why_ = "GLFW is not installed (libglfw.so.3)";
            return;
        }
        const auto symbol = [&](const char* name) { return dlsym(library_, name); };
        const auto init = reinterpret_cast<decltype(&glfwInit)>(symbol("glfwInit"));
        const auto hint = reinterpret_cast<decltype(&glfwWindowHint)>(symbol("glfwWindowHint"));
        const auto create = reinterpret_cast<decltype(&glfwCreateWindow)>(symbol("glfwCreateWindow"));
        const auto makeCurrent = reinterpret_cast<decltype(&glfwMakeContextCurrent)>(symbol("glfwMakeContextCurrent"));
        gGetProcAddress = reinterpret_cast<decltype(&glfwGetProcAddress)>(symbol("glfwGetProcAddress"));
        if (!init || !hint || !create || !makeCurrent || !gGetProcAddress) {
            why_ = "GLFW lacks a function";
            return;
        }
        if (init() != GLFW_TRUE) {
            why_ = "GLFW found no display (run the tests under xvfb-run -a)";
            return;
        }
        hint(GLFW_VISIBLE, GLFW_FALSE);
        hint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        hint(GLFW_CONTEXT_VERSION_MINOR, 3);
        hint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        hint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        hint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
        hint(GLFW_DEPTH_BITS, 0);
        hint(GLFW_STENCIL_BITS, 0);
        hint(GLFW_SAMPLES, 0);
        window_ = create(64, 64, "VeloPlay test", nullptr, nullptr);
        if (!window_) {
            why_ = "no OpenGL 3.3 core context here";
            return;
        }
        makeCurrent(window_);
        if (!gl_.load(&GlWindow::find, why_)) return;
        const auto* renderer = reinterpret_cast<const char*>(gl_.GetString(GL_RENDERER));
        std::printf("[ OpenGL    ] %s, %s\n", renderer ? renderer : "?",
                    reinterpret_cast<const char*>(gl_.GetString(GL_VERSION)));
        ok_ = true;
    }

    static void* find(const char* name) {
        return reinterpret_cast<void*>(gGetProcAddress(name));
    }

    void* library_ = nullptr;
    GLFWwindow* window_ = nullptr;
    GlFunctions gl_;
    bool ok_ = false;
    std::string why_;
};

#define VELOPLAY_NEED_GL()                                                                                             \
    if (!GlWindow::get().ok()) GTEST_SKIP() << GlWindow::get().why();                                                  \
    const GlFunctions& gl = GlWindow::get().gl();                                                                      \
    (void)gl

/// A framebuffer of any size that can be read back whole, top row first as on screen.
struct Surface {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    int width = 0;
    int height = 0;

    bool create(const GlFunctions& gl, int w, int h) {
        width = w;
        height = h;
        gl.GenTextures(1, &texture);
        gl.BindTexture(GL_TEXTURE_2D, texture);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.BindTexture(GL_TEXTURE_2D, 0);
        gl.GenFramebuffers(1, &framebuffer);
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const bool complete = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        return complete;
    }
    void destroy(const GlFunctions& gl) {
        if (framebuffer) gl.DeleteFramebuffers(1, &framebuffer);
        if (texture) gl.DeleteTextures(1, &texture);
        framebuffer = 0;
        texture = 0;
    }

    /// Binds it for drawing and reading and fills it with one grey.
    void begin(const GlFunctions& gl, float grey = 0.0f) const {
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.Viewport(0, 0, width, height);
        gl.ClearColor(grey, grey, grey, 1.0f);
        gl.Clear(GL_COLOR_BUFFER_BIT);
    }

    /// RGBA of every pixel, rows from the top of the screen down.
    [[nodiscard]] std::vector<std::uint8_t> rgba(const GlFunctions& gl) const {
        std::vector<std::uint8_t> bottomUp(static_cast<std::size_t>(width) * height * 4);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
        gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
        std::vector<std::uint8_t> out(bottomUp.size());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y) {
            std::copy_n(bottomUp.data() + static_cast<std::size_t>(height - 1 - y) * row, row,
                        out.data() + static_cast<std::size_t>(y) * row);
        }
        return out;
    }
    [[nodiscard]] std::array<int, 4> pixel(const GlFunctions& gl, int x, int y) const {
        const auto all = rgba(gl);
        const std::uint8_t* p = &all[(static_cast<std::size_t>(y) * width + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
    /// The red channel of every pixel, row by row. The test pictures are grey.
    [[nodiscard]] std::vector<int> read(const GlFunctions& gl) const {
        const auto all = rgba(gl);
        std::vector<int> out(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = all[i * 4];
        return out;
    }
    /// Blue minus red of every pixel: brightness cancels out, colour is left.
    [[nodiscard]] std::vector<int> readTint(const GlFunctions& gl) const {
        const auto all = rgba(gl);
        std::vector<int> out(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<int>(all[i * 4 + 2]) - static_cast<int>(all[i * 4]);
        return out;
    }
};

} // namespace

TEST(GlHarness, ClearsAFramebufferAndReadsItBack) {
    VELOPLAY_NEED_GL();
    Surface surface;
    ASSERT_TRUE(surface.create(gl, 16, 8));
    surface.begin(gl, 0.5f);
    const auto p = surface.pixel(gl, 3, 2);
    EXPECT_NEAR(p[0], 128, 1);
    EXPECT_EQ(gl.GetError(), static_cast<GLenum>(GL_NO_ERROR));
    surface.destroy(gl);
}

namespace {

constexpr int kTarget = 64;

/// The renderer, made on the test window's context.
#define VELOPLAY_MAKE_RENDERER(renderer)                                                                               \
    linuxos::GlVideoRenderer renderer;                                                                                 \
    {                                                                                                                  \
        std::string makeError;                                                                                         \
        ASSERT_TRUE(renderer.create(GlWindow::get().loader(), makeError)) << makeError;                               \
    }

/// What the renderer must leave behind for the window and Dear ImGui: the caller's framebuffer, no
/// pixel buffer bound, plain unpacking, and no error. A pass that fails in OpenGL draws nothing and says
/// nothing, so every test asks.
void expectCleanState(const GlFunctions& gl, GLuint framebuffer, const char* after) {
    GLint value = -1;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &value);
    EXPECT_EQ(value, static_cast<GLint>(framebuffer)) << "draw framebuffer after " << after;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &value);
    EXPECT_EQ(value, static_cast<GLint>(framebuffer)) << "read framebuffer after " << after;
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &value);
    EXPECT_EQ(value, 0) << "unpack buffer after " << after;
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value);
    EXPECT_EQ(value, 0) << "pack buffer after " << after;
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &value);
    EXPECT_EQ(value, 0) << "unpack row length after " << after;
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &value);
    EXPECT_EQ(value, 4) << "unpack alignment after " << after;
    EXPECT_FALSE(gl.IsEnabled(GL_SCISSOR_TEST)) << after;
    EXPECT_FALSE(gl.IsEnabled(GL_BLEND)) << after;
    EXPECT_EQ(gl.GetError(), static_cast<GLenum>(GL_NO_ERROR)) << "after " << after;
}

/// A picture of one colour in processor memory. `pad` adds bytes of rubbish at the end of every row, as a
/// decoder's buffers have.
struct SolidFrame {
    std::vector<std::uint8_t> luma, u, v, uv;
    media::VideoFrame frame;
    int lumaStride = 0;

    SolidFrame(int width, int height, int y, int cb, int cr, bool nv12, int pad = 0) {
        const int cw = (width + 1) / 2;
        const int ch = (height + 1) / 2;
        lumaStride = width + pad;
        luma.assign(static_cast<std::size_t>(lumaStride * height), 0xA5);
        for (int row = 0; row < height; ++row) {
            std::fill_n(luma.begin() + row * lumaStride, width, static_cast<std::uint8_t>(y));
        }
        frame.width = width;
        frame.height = height;
        frame.planes[0] = luma.data();
        frame.strides[0] = lumaStride;
        if (nv12) {
            const int stride = cw * 2 + pad;
            uv.assign(static_cast<std::size_t>(stride * ch), 0x3C);
            for (int row = 0; row < ch; ++row) {
                for (int x = 0; x < cw; ++x) {
                    uv[static_cast<std::size_t>(row * stride + x * 2)] = static_cast<std::uint8_t>(cb);
                    uv[static_cast<std::size_t>(row * stride + x * 2 + 1)] = static_cast<std::uint8_t>(cr);
                }
            }
            frame.layout = media::PixelLayout::Nv12;
            frame.planes[1] = uv.data();
            frame.strides[1] = stride;
        } else {
            const int stride = cw + pad;
            u.assign(static_cast<std::size_t>(stride * ch), 0x3C);
            v.assign(static_cast<std::size_t>(stride * ch), 0xC3);
            for (int row = 0; row < ch; ++row) {
                std::fill_n(u.begin() + row * stride, cw, static_cast<std::uint8_t>(cb));
                std::fill_n(v.begin() + row * stride, cw, static_cast<std::uint8_t>(cr));
            }
            frame.layout = media::PixelLayout::I420;
            frame.planes[1] = u.data();
            frame.planes[2] = v.data();
            frame.strides[1] = stride;
            frame.strides[2] = stride;
        }
    }
    void setLuma(int x, int y, int value) { luma[static_cast<std::size_t>(y * lumaStride + x)] = static_cast<std::uint8_t>(value); }
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

/// A grey picture of that structure, moved by (dx, dy) pixels. The same as on Windows.
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
        point();
        frame.strides[0] = width;
        frame.strides[1] = cw;
        frame.strides[2] = cw;
    }
    PatternFrame(const PatternFrame& other) : luma(other.luma), u(other.u), v(other.v), frame(other.frame) { point(); }
    void point() {
        frame.planes[0] = luma.data();
        frame.planes[1] = u.data();
        frame.planes[2] = v.data();
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

media::FrameChoice between(std::uint64_t base, std::uint64_t next, int step, int steps) {
    media::FrameChoice choice;
    choice.valid = true;
    choice.base = base;
    choice.next = next;
    choice.step = step;
    choice.steps = steps;
    return choice;
}

} // namespace

TEST(GlVideoRenderer, ConvertsAndLetterboxesACpuPicture) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);
    EXPECT_FALSE(renderer.hasPicture());

    // Nothing to draw yet: the target keeps its clear colour.
    canvas.begin(gl, 0.5f);
    renderer.draw(kTarget, kTarget);
    EXPECT_NEAR(canvas.pixel(gl, 32, 32)[0], 128, 2);

    // Pure red in BT.709 with limited range, twice as wide as high.
    SolidFrame red(32, 16, 63, 102, 240, false);
    canvas.begin(gl, 0.5f);
    ASSERT_TRUE(renderer.submit(red.frame));
    expectCleanState(gl, canvas.framebuffer, "submit");
    EXPECT_TRUE(renderer.hasPicture());
    renderer.draw(kTarget, kTarget);
    expectCleanState(gl, canvas.framebuffer, "draw");
    GLint viewport[4] = {};
    gl.GetIntegerv(GL_VIEWPORT, viewport);
    EXPECT_EQ(viewport[2], kTarget) << "the caller's viewport comes back";

    const auto centre = canvas.pixel(gl, 32, 32);
    EXPECT_NEAR(centre[0], 255, 4);
    EXPECT_NEAR(centre[1], 0, 4);
    EXPECT_NEAR(centre[2], 0, 4);
    // The picture fills the width and the middle half of the height.
    EXPECT_NEAR(canvas.pixel(gl, 2, 32)[0], 255, 4);
    EXPECT_NEAR(canvas.pixel(gl, 61, 32)[0], 255, 4);
    EXPECT_NEAR(canvas.pixel(gl, 32, 18)[0], 255, 4);
    EXPECT_NEAR(canvas.pixel(gl, 32, 45)[0], 255, 4);
    // Above and below it the target is untouched.
    EXPECT_NEAR(canvas.pixel(gl, 32, 8)[0], 128, 2);
    EXPECT_NEAR(canvas.pixel(gl, 32, 8)[1], 128, 2);
    EXPECT_NEAR(canvas.pixel(gl, 32, 56)[1], 128, 2);
    EXPECT_EQ(renderer.shownWidth(), 64);
    EXPECT_EQ(renderer.shownHeight(), 32);
    EXPECT_EQ(renderer.shownTop(), 16);
    canvas.destroy(gl);
}

TEST(GlVideoRenderer, TakesNv12AndFollowsFormatChanges) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);

    // White, as a portrait picture in the NV12 layout.
    SolidFrame white(16, 32, 235, 128, 128, true);
    ASSERT_TRUE(renderer.submit(white.frame));
    canvas.begin(gl, 0.5f);
    renderer.draw(kTarget, kTarget);
    const auto centre = canvas.pixel(gl, 32, 32);
    EXPECT_NEAR(centre[0], 255, 3);
    EXPECT_NEAR(centre[1], 255, 3);
    EXPECT_NEAR(centre[2], 255, 3);
    EXPECT_NEAR(canvas.pixel(gl, 4, 32)[0], 128, 2); // bars left and right this time

    // The stream switches to another size and to full-range BT.601: blue.
    SolidFrame blue(48, 48, 29, 255, 107, false);
    blue.frame.color = {true, true};
    ASSERT_TRUE(renderer.submit(blue.frame));
    EXPECT_EQ(renderer.width(), 48);
    canvas.begin(gl, 0.5f);
    renderer.draw(kTarget, kTarget);
    const auto filled = canvas.pixel(gl, 5, 5);
    EXPECT_NEAR(filled[0], 0, 6);
    EXPECT_NEAR(filled[1], 0, 6);
    EXPECT_NEAR(filled[2], 255, 6);

    renderer.clear();
    EXPECT_FALSE(renderer.hasPicture());
    canvas.begin(gl, 0.5f);
    renderer.draw(kTarget, kTarget);
    EXPECT_NEAR(canvas.pixel(gl, 32, 32)[2], 128, 2);

    // Pictures the renderer cannot take are refused, not drawn half.
    media::VideoFrame empty;
    EXPECT_FALSE(renderer.submit(empty));
    media::VideoFrame onCard;
    onCard.width = 16;
    onCard.height = 16;
    onCard.layout = media::PixelLayout::D3D11Nv12; // a Windows decoder's surface: no way here
    EXPECT_FALSE(renderer.submit(onCard));
    expectCleanState(gl, canvas.framebuffer, "refused pictures");
    canvas.destroy(gl);
}

// The Windows tests always have rows exactly as long as the picture. A decoder's rows are longer, with
// rubbish at the end, and pictures can have odd sizes. The same colour has to come out of I420 and NV12.
TEST(GlVideoRenderer, ReadsRowsWithGapsOddSizesAndBothLayoutsAlike) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, 33, 17));
    VELOPLAY_MAKE_RENDERER(renderer);
    // An orange: U below the middle, V above it. Swapped planes would give a blue.
    const int y = 140, cb = 80, cr = 190;
    std::array<int, 4> seen[6]{};
    int index = 0;
    for (const bool nv12 : {false, true}) {
        // 13 makes NV12 rows an odd number of bytes long (copied row by row); 14 is a decoder's usual even gap.
        for (const int pad : {0, 13, 14}) {
            SolidFrame picture(33, 17, y, cb, cr, nv12, pad);
            ASSERT_TRUE(renderer.submit(picture.frame)) << (nv12 ? "NV12" : "I420") << " pad " << pad;
            canvas.begin(gl);
            renderer.draw(33, 17);
            expectCleanState(gl, canvas.framebuffer, "draw");
            const auto all = canvas.rgba(gl);
            // Every pixel the same, the last column and row too: the rubbish never shows.
            int worst = 0;
            for (int p = 0; p < 33 * 17; ++p) {
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(all[p * 4 + c] - all[c]));
            }
            EXPECT_LE(worst, 1) << (nv12 ? "NV12" : "I420") << " pad " << pad;
            seen[index++] = {all[0], all[1], all[2], all[3]};
        }
    }
    const media::ColorMatrix m = media::yuvToRgb({});
    const double yuv[4] = {y / 255.0, cb / 255.0, cr / 255.0, 1.0};
    for (int c = 0; c < 3; ++c) {
        double value = 0.0;
        for (int k = 0; k < 4; ++k) value += m.rows[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] * yuv[k];
        const int expected = static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
        for (const auto& colour : seen) EXPECT_NEAR(colour[c], expected, 2) << "channel " << c;
    }
    EXPECT_GT(seen[0][0], seen[0][2] + 60) << "orange, not blue";
    canvas.destroy(gl);
}

// The ported tests are all the same upside down. This one is not: the top of the picture is dark, the
// bottom light, and it has to arrive the right way up on every path, in a target with an odd margin.
TEST(GlVideoRenderer, ShowsThePictureTheRightWayUpOnEveryPath) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 64;
    constexpr int kHeight = 65;
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kWidth, kHeight));
    VELOPLAY_MAKE_RENDERER(renderer);
    SolidFrame picture(32, 20, 50, 128, 128, false);
    for (int y = 10; y < 20; ++y) {
        for (int x = 0; x < 32; ++x) picture.setLuma(x, y, 200);
    }
    ASSERT_TRUE(renderer.submit(picture.frame));
    struct Path {
        const char* name;
        bool upscale;
        float sharpness;
    };
    for (const Path& path : {Path{"soft", false, 0.0f}, Path{"Lanczos", true, 0.0f}, Path{"sharpened", true, 1.0f}}) {
        SCOPED_TRACE(path.name);
        linuxos::GlVideoRenderer::Output output;
        output.upscale = path.upscale;
        output.sharpness = path.sharpness;
        canvas.begin(gl, 0.5f);
        renderer.draw(kWidth, kHeight, output);
        expectCleanState(gl, canvas.framebuffer, path.name);
        // 64 x 40, centred: 12 rows above, 13 below, as Direct3D rounds it.
        EXPECT_EQ(renderer.shownTop(), 12);
        EXPECT_EQ(renderer.shownHeight(), 40);
        const auto rows = canvas.read(gl);
        const auto at = [&](int x, int y) { return rows[static_cast<std::size_t>(y * kWidth + x)]; };
        EXPECT_NEAR(at(32, 11), 128, 2) << "above the picture";
        EXPECT_LT(at(32, 12), 80) << "first row of the picture: its top, dark";
        EXPECT_LT(at(32, 25), 80);
        EXPECT_GT(at(32, 40), 190);
        EXPECT_GT(at(32, 51), 190) << "last row of the picture: its bottom, light";
        EXPECT_NEAR(at(32, 52), 128, 2) << "below the picture";
    }

    // And a screenshot of that region has the picture's top in its first row.
    linuxos::GlFrameGrabber grabber(&renderer.gl());
    canvas.begin(gl, 0.5f);
    renderer.draw(kWidth, kHeight);
    grabber.request();
    grabber.capture(renderer.shownLeft(), renderer.shownTop(), renderer.shownWidth(), renderer.shownHeight(), kHeight);
    expectCleanState(gl, canvas.framebuffer, "capture");
    std::optional<media::Image> image;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!image && std::chrono::steady_clock::now() < deadline) {
        image = grabber.take();
        if (!image) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(image.has_value()) << grabber.failure();
    ASSERT_EQ(image->width, 64);
    ASSERT_EQ(image->height, 40);
    EXPECT_LT(image->bgra[(0 * 64 + 32) * 4 + 2], 80) << "first row dark";
    EXPECT_GT(image->bgra[(39 * 64 + 32) * 4 + 2], 190) << "last row light";
    canvas.destroy(gl);
}

TEST(GlVideoRenderer, TellsARepeatedPictureFromANewOne) {
    VELOPLAY_NEED_GL();
    VELOPLAY_MAKE_RENDERER(renderer);
    // A game at 30 fps in a 60 fps stream: every picture comes twice. The
    // second time it went through the encoder again and is a level off here and there.
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    std::map<std::uint64_t, double> shares; // by picture, counted from 1
    std::map<std::uint64_t, double> means;
    const auto gather = [&] {
        while (const auto change = renderer.takeChange()) {
            shares[change->picture] = change->share;
            means[change->picture] = change->mean;
        }
    };
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
        if (i % 2 == 1) {
            EXPECT_EQ(renderer.newestId(), shownBefore);
        }
        // In the program a picture comes every 16 ms or so; the comparisons are read without waiting.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
        while (std::chrono::steady_clock::now() < until) {
            gather();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (shares.size() < 11 && std::chrono::steady_clock::now() < deadline) {
        gather();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("picture comparisons read back: %zu of 11\n", shares.size());
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
    media::CadenceDetector detector;
    for (std::uint64_t picture = 2; picture < 26; ++picture) detector.add(picture, picture % 2 == 0 ? repeated : fresh);
    EXPECT_TRUE(detector.halved());
    EXPECT_TRUE(detector.isRepeat(100));
}

TEST(GlVideoRenderer, ShowsThePictureThatIsAskedFor) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);

    SolidFrame dark(64, 64, 60, 128, 128, false);
    SolidFrame light(64, 64, 200, 128, 128, false);
    ASSERT_TRUE(renderer.submit(dark.frame));
    const std::uint64_t darkId = renderer.newestId();
    ASSERT_TRUE(renderer.submit(light.frame));
    const std::uint64_t lightId = renderer.newestId();
    EXPECT_GT(lightId, darkId);

    linuxos::GlVideoRenderer::Output output;
    canvas.begin(gl);
    renderer.draw(kTarget, kTarget, output); // nothing asked for: the newest
    const int lightGrey = canvas.pixel(gl, 32, 32)[0];
    output.choice.valid = true;
    output.choice.base = darkId;
    canvas.begin(gl);
    renderer.draw(kTarget, kTarget, output);
    const int darkGrey = canvas.pixel(gl, 32, 32)[0];
    EXPECT_GT(lightGrey, darkGrey + 100);

    // Halfway between two flat pictures is the grey in the middle.
    output.choice = between(darkId, lightId, 1, 2);
    canvas.begin(gl);
    renderer.draw(kTarget, kTarget, output);
    if (renderer.frameGenerationAvailable()) {
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        EXPECT_NEAR(canvas.pixel(gl, 32, 32)[0], (lightGrey + darkGrey) / 2, 3);
    }
    expectCleanState(gl, canvas.framebuffer, "a computed picture");

    // A picture that is gone cannot be shown; the newest one is.
    output.choice = {};
    output.choice.valid = true;
    output.choice.base = 12345;
    canvas.begin(gl);
    renderer.draw(kTarget, kTarget, output);
    EXPECT_NEAR(canvas.pixel(gl, 32, 32)[0], lightGrey, 2);
    canvas.destroy(gl);
}

TEST(GlVideoRenderer, EnlargesCleanlyAndSharpensEdges) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);

    // Left half dark, right half light, shown at twice the size.
    SolidFrame picture(32, 32, 80, 128, 128, false);
    for (int y = 0; y < 32; ++y) {
        for (int x = 16; x < 32; ++x) picture.setLuma(x, y, 170);
    }
    ASSERT_TRUE(renderer.submit(picture.frame));

    const auto row = [&](bool upscale, float sharpness) {
        linuxos::GlVideoRenderer::Output output;
        output.upscale = upscale;
        output.sharpness = sharpness;
        canvas.begin(gl);
        renderer.draw(kTarget, kTarget, output);
        const auto all = canvas.read(gl);
        std::array<int, kTarget> values{};
        for (int x = 0; x < kTarget; ++x) values[static_cast<std::size_t>(x)] = all[static_cast<std::size_t>(32 * kTarget + x)];
        return values;
    };
    const auto soft = row(false, 0.0f);
    EXPECT_STREQ(renderer.scalerName(), "");
    const auto lanczos = row(true, 0.0f);
    EXPECT_STREQ(renderer.scalerName(), "Lanczos");
    const auto sharpened = row(true, 1.0f);
    const int dark = soft[8];
    const int light = soft[56];
    EXPECT_GT(light, dark + 80);

    for (const auto& values : {lanczos, sharpened}) {
        EXPECT_NEAR(values[8], dark, 2);
        EXPECT_NEAR(values[56], light, 2);
    }
    for (int x = 0; x < kTarget; ++x) {
        EXPECT_GE(lanczos[static_cast<std::size_t>(x)], dark - 2) << "at " << x;
        EXPECT_LE(lanczos[static_cast<std::size_t>(x)], light + 2) << "at " << x;
    }
    const auto steepest = [](const std::array<int, kTarget>& values) {
        int best = 0;
        for (std::size_t x = 24; x < 40; ++x) best = std::max(best, values[x + 1] - values[x]);
        return best;
    };
    std::printf("edge step: soft %d, Lanczos %d, sharpened %d\n", steepest(soft), steepest(lanczos), steepest(sharpened));
    EXPECT_GE(steepest(lanczos), steepest(soft));
    EXPECT_GT(steepest(sharpened), steepest(lanczos));
    canvas.destroy(gl);
}

TEST(GlVideoRenderer, SmoothsBandingAndLeavesDetailAlone) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);

    const auto shown = [&](const SolidFrame& picture, bool deband) {
        EXPECT_TRUE(renderer.submit(picture.frame));
        linuxos::GlVideoRenderer::Output output;
        output.deband = deband;
        canvas.begin(gl);
        renderer.draw(kTarget, kTarget, output);
        EXPECT_EQ(renderer.debanded(), deband);
        return canvas.read(gl);
    };
    const auto columnMean = [](const std::vector<int>& red, int x) {
        double sum = 0.0;
        for (int y = 8; y < kTarget - 8; ++y) sum += red[static_cast<std::size_t>(y * kTarget + x)];
        return sum / (kTarget - 16);
    };

    for (const int step : {1, 2}) {
        SolidFrame steps(kTarget, kTarget, 100, 128, 128, false);
        for (int y = 0; y < kTarget; ++y) {
            for (int x = 0; x < kTarget; ++x) steps.setLuma(x, y, 100 + step * (x / 16));
        }
        const auto banded = shown(steps, false);
        const auto smoothed = shown(steps, true);
        double before = 0.0, after = 0.0, largestJumpBefore = 0.0, largestJumpAfter = 0.0;
        const double first = columnMean(banded, 8);
        const double slope = (columnMean(banded, 56) - first) / 48.0;
        for (int x = 16; x < 48; ++x) {
            const double ramp = first + slope * (static_cast<double>(x) - 8.0);
            before += std::abs(columnMean(banded, x) - ramp);
            after += std::abs(columnMean(smoothed, x) - ramp);
            largestJumpBefore = std::max(largestJumpBefore, std::abs(columnMean(banded, x + 1) - columnMean(banded, x)));
            largestJumpAfter = std::max(largestJumpAfter, std::abs(columnMean(smoothed, x + 1) - columnMean(smoothed, x)));
        }
        std::printf("banding in steps of %d: off the ramp by %.2f levels before, %.2f after; largest step %.2f before, "
                    "%.2f after\n",
                    step, before / 32.0, after / 32.0, largestJumpBefore, largestJumpAfter);
        EXPECT_GT(largestJumpBefore, 0.8 * step) << "steps of " << step;
        EXPECT_LT(after, before * 0.7) << "steps of " << step;
        EXPECT_LT(largestJumpAfter, largestJumpBefore * 0.6) << "steps of " << step;
    }
    {
        SolidFrame steps(kTarget, kTarget, 100, 128, 128, false);
        for (int y = 0; y < kTarget; ++y) {
            for (int x = 0; x < kTarget; ++x) steps.setLuma(x, y, 100 + x / 16);
        }
        EXPECT_EQ(shown(steps, true), shown(steps, true)) << "a still picture must not shimmer";
    }
    SolidFrame checker(kTarget, kTarget, 100, 128, 128, false);
    for (int y = 0; y < kTarget; ++y) {
        for (int x = 0; x < kTarget; ++x) checker.setLuma(x, y, ((x / 2 + y / 2) % 2) ? 130 : 100);
    }
    const auto plainChecker = shown(checker, false);
    const auto debandedChecker = shown(checker, true);
    int checkerOff = 0;
    double checkerMean = 0.0;
    for (std::size_t i = 0; i < plainChecker.size(); ++i) {
        checkerOff = std::max(checkerOff, std::abs(plainChecker[i] - debandedChecker[i]));
        checkerMean += std::abs(plainChecker[i] - debandedChecker[i]);
    }
    checkerMean /= static_cast<double>(plainChecker.size());
    std::printf("detail: largest change %d levels, mean %.2f\n", checkerOff, checkerMean);
    EXPECT_LE(checkerOff, 3) << "fine detail was smoothed";
    EXPECT_LT(checkerMean, 0.5) << "fine detail was smoothed";

    SolidFrame edge(kTarget, kTarget, 60, 128, 128, false);
    for (int y = 0; y < kTarget; ++y) {
        for (int x = kTarget / 2; x < kTarget; ++x) edge.setLuma(x, y, 180);
    }
    const auto plainEdge = shown(edge, false);
    const auto debandedEdge = shown(edge, true);
    int edgeOff = 0;
    for (std::size_t i = 0; i < plainEdge.size(); ++i) edgeOff = std::max(edgeOff, std::abs(plainEdge[i] - debandedEdge[i]));
    EXPECT_LE(edgeOff, 1) << "the edge was softened";

    SolidFrame flat(kTarget, kTarget, 120, 128, 128, false);
    EXPECT_EQ(shown(flat, false), shown(flat, true));
    canvas.destroy(gl);
}

TEST(GlVideoRenderer, GivesThePictureAStyleAndLeavesItAloneWithout) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);
    SolidFrame dull(64, 64, 150, 118, 138, false);
    ASSERT_TRUE(renderer.submit(dull.frame));
    const auto drawWith = [&](int look, float strength) {
        linuxos::GlVideoRenderer::Output output;
        output.look = look;
        output.lookStrength = strength;
        canvas.begin(gl);
        renderer.draw(kTarget, kTarget, output);
        return std::make_pair(canvas.pixel(gl, 32, 32), canvas.pixel(gl, 1, 1));
    };
    const auto spread = [](const std::array<int, 4>& c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); };

    const auto [plainCentre, plainCorner] = drawWith(0, 1.0f);
    EXPECT_FALSE(renderer.styled());
    const auto [vividCentre, vividCorner] = drawWith(1, 1.0f);
    EXPECT_TRUE(renderer.styled());
    EXPECT_GT(spread(vividCentre), spread(plainCentre) + 3) << "vivid makes the dull colour more colourful";
    const auto [cinemaCentre, cinemaCorner] = drawWith(2, 1.0f);
    EXPECT_LT(cinemaCorner[0] + cinemaCorner[1] + cinemaCorner[2], cinemaCentre[0] + cinemaCentre[1] + cinemaCentre[2] - 15)
        << "the corners get darker";
    EXPECT_GT(cinemaCentre[0] - cinemaCentre[2], plainCentre[0] - plainCentre[2]) << "the light parts get warmer";
    const auto [noneCentre, noneCorner] = drawWith(2, 0.0f);
    for (int i = 0; i < 3; ++i) EXPECT_NEAR(noneCentre[static_cast<std::size_t>(i)], plainCentre[static_cast<std::size_t>(i)], 1);

    SolidFrame white(64, 64, 235, 128, 128, false);
    ASSERT_TRUE(renderer.submit(white.frame));
    for (const int look : {1, 2}) {
        EXPECT_GE(drawWith(look, 1.0f).first[1], 250) << "look " << look;
    }
    SolidFrame black(64, 64, 16, 128, 128, false);
    ASSERT_TRUE(renderer.submit(black.frame));
    for (const int look : {1, 2}) {
        EXPECT_LE(drawWith(look, 1.0f).first[1], 6) << "look " << look;
    }
    canvas.destroy(gl);
}

TEST(GlVideoRenderer, StartsAgainAfterReset) {
    VELOPLAY_NEED_GL();
    Surface canvas;
    ASSERT_TRUE(canvas.create(gl, kTarget, kTarget));
    linuxos::GlVideoRenderer renderer;
    SolidFrame red(32, 16, 63, 102, 240, false);
    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE(round);
        std::string error;
        ASSERT_TRUE(renderer.create(GlWindow::get().loader(), error)) << error;
        ASSERT_TRUE(renderer.submit(red.frame));
        canvas.begin(gl);
        renderer.draw(kTarget, kTarget);
        EXPECT_NEAR(canvas.pixel(gl, 32, 32)[0], 255, 4);
        renderer.reset();
        EXPECT_FALSE(renderer.hasPicture());
        EXPECT_FALSE(renderer.submit(red.frame)) << "nothing works between reset() and create()";
    }
    expectCleanState(gl, canvas.framebuffer, "reset");
    canvas.destroy(gl);
}

TEST(GlFrameGrabber, CopiesThePartOfTheScreenThatShowsThePicture) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 64;
    constexpr int kHeight = 40;
    Surface screen;
    ASSERT_TRUE(screen.create(gl, kWidth, kHeight));
    // Every pixel says where it is on screen: red = x, green = y from the top, blue = 200. OpenGL keeps
    // the bottom row first.
    std::vector<std::uint8_t> pixels(kWidth * kHeight * 4);
    for (int row = 0; row < kHeight; ++row) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* at = &pixels[static_cast<std::size_t>((row * kWidth + x) * 4)];
            at[0] = static_cast<std::uint8_t>(x);
            at[1] = static_cast<std::uint8_t>(kHeight - 1 - row);
            at[2] = 200;
            at[3] = 7; // whatever a framebuffer holds there
        }
    }
    gl.BindTexture(GL_TEXTURE_2D, screen.texture);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl.BindTexture(GL_TEXTURE_2D, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, screen.framebuffer);

    linuxos::GlFrameGrabber grabber(&gl);
    grabber.capture(10, 5, 40, 30, kHeight); // without a request nothing is copied
    EXPECT_FALSE(grabber.busy());
    EXPECT_FALSE(grabber.take().has_value());

    grabber.request();
    grabber.capture(10, 5, 40, 30, kHeight);
    expectCleanState(gl, screen.framebuffer, "capture");
    std::optional<media::Image> image;
    for (int frame = 0; frame < 2000 && !image; ++frame) {
        image = grabber.take();
        if (!image) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(image.has_value()) << grabber.failure();
    EXPECT_FALSE(grabber.busy());
    ASSERT_EQ(image->width, 40);
    ASSERT_EQ(image->height, 30);
    for (const auto& [x, y] : {std::pair{0, 0}, std::pair{39, 29}, std::pair{17, 3}}) {
        const std::uint8_t* at = &image->bgra[static_cast<std::size_t>((y * 40 + x) * 4)];
        EXPECT_EQ(at[2], 10 + x) << "red at " << x << "," << y;
        EXPECT_EQ(at[1], 5 + y) << "green at " << x << "," << y;
        EXPECT_EQ(at[0], 200);
        EXPECT_EQ(at[3], 255);
    }
    EXPECT_TRUE(media::encodePng(*image).ok());
    expectCleanState(gl, screen.framebuffer, "take");
    grabber.release();
    screen.destroy(gl);
}

TEST(GlFrameStream, HandsEveryPictureOverInOrderWithItsKind) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 32;
    constexpr int kHeight = 24;
    Surface screen;
    ASSERT_TRUE(screen.create(gl, kWidth, kHeight));
    linuxos::GlFrameStream stream(&gl);
    const auto start = std::chrono::steady_clock::now();
    constexpr media::ShownKind kinds[] = {media::ShownKind::Real, media::ShownKind::Computed, media::ShownKind::Repeated};
    std::vector<linuxos::GlFrameStream::Picture> taken;
    constexpr int kPictures = 9;
    int sent = 0;
    // Like the window: one picture per frame, and whatever has arrived is taken right after.
    for (int frame = 0; frame < 2000 && static_cast<int>(taken.size()) < kPictures; ++frame) {
        if (sent < kPictures) {
            // Each picture has its own shade of red, so the order can be read back.
            gl.BindFramebuffer(GL_FRAMEBUFFER, screen.framebuffer);
            gl.Viewport(0, 0, kWidth, kHeight);
            gl.ClearColor(static_cast<float>(20 * (sent + 1)) / 255.0f, 0.0f, 0.5f, 1.0f);
            gl.Clear(GL_COLOR_BUFFER_BIT);
            if (stream.capture(4, 2, 16, 12, kHeight, kinds[sent % 3], start + std::chrono::milliseconds(16 * sent))) {
                ++sent;
            }
        }
        while (auto picture = stream.take({})) taken.push_back(std::move(*picture));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(static_cast<int>(taken.size()), kPictures) << stream.failure();
    for (int i = 0; i < kPictures; ++i) {
        ASSERT_EQ(taken[static_cast<std::size_t>(i)].bgra.size(), 16u * 12u * 4u);
        EXPECT_EQ(taken[static_cast<std::size_t>(i)].kind, kinds[i % 3]) << i;
        EXPECT_EQ(taken[static_cast<std::size_t>(i)].at, start + std::chrono::milliseconds(16 * i)) << i;
        EXPECT_NEAR(taken[static_cast<std::size_t>(i)].bgra[2], 20 * (i + 1), 1) << "red of picture " << i;
        EXPECT_NEAR(taken[static_cast<std::size_t>(i)].bgra[0], 128, 1) << "blue of picture " << i;
    }
    expectCleanState(gl, screen.framebuffer, "recording");
    stream.reset();
    screen.destroy(gl);
}

// ---- Frame generation ---------------------------------------------------------
//
// The scenes of test_platform_win.cpp, picture for picture. The numbers they
// print can be laid next to those from Windows: the method and its constants
// are the same, so a clear difference is a fault of the port.

namespace {

/// A ladder in front of a background, both moving with the camera: bright rungs sixteen pixels apart.
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

/// Something blue in front of grey that is exactly as bright: the nozzle at the petrol station.
struct TintedShapeFrame {
    PatternFrame picture;

    static bool inside(double x, double y, int width, int height) {
        const double ex = (x - width * 0.56) / (width * 0.17);
        const double ey = (y - height * 0.42) / (height * 0.27);
        if (ex * ex + ey * ey < 1.0) return true;
        const double along = (x - width * 0.30) * 0.8 - (y - height * 0.80) * 0.6;
        const double across = (x - width * 0.30) * 0.6 + (y - height * 0.80) * 0.8;
        return along > 0.0 && along < width * 0.30 && std::abs(across) < height * 0.07;
    }

    TintedShapeFrame(int width, int height, double dx, double dy, int grain, bool hand) : picture(width, height, dx, dy) {
        const double handX = width * 0.36 + dx;
        const double handY = height * 0.58 + dy;
        const double handRadius = height * 0.13;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                auto& value = picture.luma[static_cast<std::size_t>(y * width + x)];
                const double hx = x + 0.5 - handX;
                const double hy = y + 0.5 - handY;
                if (hand && hx * hx + hy * hy < handRadius * handRadius) continue;
                value = static_cast<std::uint8_t>(
                    std::lround(78.0 + 5.0 * (noise(x, y, 5.0, grain) - 0.5) + 2.0 * (cell(x, y, grain + 50) - 0.5)));
            }
        }
        const int cw = (width + 1) / 2;
        const int ch = (height + 1) / 2;
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
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

struct Generated {
    double error = 0.0;
    double mixError = 0.0;
    double tintError = 0.0;
    double tintMixError = 0.0;
    double milliseconds = 0.0;
};

/// How far the picture computed between `first` and `second` is from `expected`, and how long computing
/// one takes, from the upload of the second picture to the computed one read back.
Generated generateBetween(const GlFunctions& gl, Surface& surface, linuxos::GlVideoRenderer& renderer,
                          const media::VideoFrame& first, const media::VideoFrame& second,
                          const media::VideoFrame& expected, int width, int height, int rounds = 12) {
    const auto show = [&](const media::VideoFrame& frame) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(frame));
        surface.begin(gl);
        renderer.draw(width, height);
        return surface.read(gl);
    };
    const std::vector<int> wanted = show(expected);
    const std::vector<int> wantedTint = surface.readTint(gl);
    const std::vector<int> a = show(first);
    const std::vector<int> aTint = surface.readTint(gl);
    const std::vector<int> b = show(second);
    const std::vector<int> bTint = surface.readTint(gl);

    Generated result;
    std::vector<int> betweenPicture;
    double best = 1e9;
    for (int round = 0; round < std::max(1, rounds); ++round) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(first));
        const std::uint64_t firstId = renderer.newestId();
        surface.begin(gl);
        renderer.draw(width, height);
        (void)surface.read(gl); // the card is done with everything before the clock starts
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(renderer.submit(second));
        linuxos::GlVideoRenderer::Output output;
        output.choice = between(firstId, renderer.newestId(), 1, 2);
        surface.begin(gl);
        renderer.draw(width, height, output);
        betweenPicture = surface.read(gl); // waits for the card
        const auto end = std::chrono::steady_clock::now();
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
    }
    const std::vector<int> betweenTint = surface.readTint(gl);
    std::vector<int> mixed(a.size());
    std::vector<int> mixedTint(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        mixed[i] = (a[i] + b[i] + 1) / 2;
        mixedTint[i] = (aTint[i] + bTint[i]) / 2;
    }
    result.error = meanDifference(betweenPicture, wanted, width, height, 24);
    result.mixError = meanDifference(mixed, wanted, width, height, 24);
    result.tintError = meanDifference(betweenTint, wantedTint, width, height, 24);
    result.tintMixError = meanDifference(mixedTint, wantedTint, width, height, 24);
    result.milliseconds = best;
    return result;
}

/// The menu of a game: a backdrop that stands, and on it a panel with lines of writing, one of them marked.
struct MenuFrame {
    PatternFrame picture;

    static bool inPanel(int x, int y, int width, int height) {
        return x >= width * 20 / 100 && x < width * 80 / 100 && y >= height * 20 / 100 && y < height * 75 / 100;
    }

    MenuFrame(int width, int height, int page) : picture(width, height, 0, 0) {
        const int left = width * 20 / 100;
        const int top = height * 20 / 100;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                auto& value = picture.luma[static_cast<std::size_t>(y * width + x)];
                if (!inPanel(x, y, width, height)) {
                    value = static_cast<std::uint8_t>(16 + (value - 16) * 6 / 10);
                    continue;
                }
                const int row = (y - top) / 10;
                const int inRow = (y - top) % 10;
                const int letter = (x - left) / 4;
                const int inLetter = (x - left) % 4;
                int shade = row == 1 + page * 2 ? 112 : 46;
                const bool word = cell(letter / 6, row, page * 7 + 1) > 0.3;
                if (word && inRow >= 3 && inRow < 7 && inLetter < 3 &&
                    cell(letter * 4 + inLetter, row * 4 + inRow, page * 7 + 2) > 0.35) {
                    shade = 215;
                }
                value = static_cast<std::uint8_t>(shade);
            }
        }
    }
};

struct PageSwitch {
    double panelFromNew = 0.0;
    double panelApart = 0.0;
    double restFromOld = 0.0;
};
PageSwitch switchMenuPage(const GlFunctions& gl, Surface& surface, linuxos::GlVideoRenderer& renderer, int width,
                          int height) {
    const auto show = [&](const media::VideoFrame& frame) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(frame));
        surface.begin(gl);
        renderer.draw(width, height);
        return surface.read(gl);
    };
    MenuFrame oldPage(width, height, 0);
    MenuFrame newPage(width, height, 1);
    const std::vector<int> a = show(oldPage.picture.frame);
    const std::vector<int> b = show(newPage.picture.frame);
    renderer.clear();

    constexpr int kPictures = 14; // the menu has been open for a moment before the page changes
    std::uint64_t before = 0;
    std::vector<int> computed;
    for (int i = 0; i < kPictures; ++i) {
        const bool last = i == kPictures - 1;
        EXPECT_TRUE(renderer.submit(last ? newPage.picture.frame : oldPage.picture.frame));
        if (i > 0) {
            linuxos::GlVideoRenderer::Output output;
            output.choice = between(before, renderer.newestId(), 1, 2);
            surface.begin(gl);
            renderer.draw(width, height, output);
            EXPECT_TRUE(renderer.lastDrawWasGenerated());
            if (last) computed = surface.read(gl);
        }
        before = renderer.newestId();
    }
    PageSwitch result;
    double panel = 0.0, apart = 0.0, rest = 0.0;
    int panelCount = 0, restCount = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y * width + x);
            if (MenuFrame::inPanel(x, y, width, height)) {
                panel += std::abs(computed[at] - b[at]);
                apart += std::abs(a[at] - b[at]);
                ++panelCount;
            } else {
                rest += std::abs(computed[at] - a[at]);
                ++restCount;
            }
        }
    }
    result.panelFromNew = panel / panelCount;
    result.panelApart = apart / panelCount;
    result.restFromOld = rest / restCount;
    return result;
}

/// A scene that stands, and a patch with structure of its own crossing it.
struct PassingFrame {
    PatternFrame picture;

    PassingFrame(int width, int height, double position) : picture(width, height, 0, 0) {
        for (int y = height / 2 - 24; y < height / 2 + 24; ++y) {
            for (int x = 0; x < width; ++x) {
                const double inPatch = x + 0.5 - position;
                if (inPatch < 0.0 || inPatch >= 48.0) continue;
                const double value = 0.15 + 0.7 * (0.6 * noise(inPatch + 3000.0, y + 7000.5, 24.0, 1) +
                                                   0.4 * noise(inPatch + 3000.0, y + 7000.5, 7.0, 2));
                picture.luma[static_cast<std::size_t>(y * width + x)] = static_cast<std::uint8_t>(std::lround(16.0 + 219.0 * value));
            }
        }
    }
};

/// A scene that moves behind something that does not: a block of fine upright bars like writing.
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

/// Hands `count` pictures made by `make(i)` in turn and computes the one halfway between each pair; gives
/// back the last of those.
template <typename Make>
std::vector<int> runSequence(const GlFunctions& gl, Surface& surface, linuxos::GlVideoRenderer& renderer, int count,
                             Make make) {
    std::uint64_t before = 0;
    std::vector<int> computed;
    for (int i = 0; i < count; ++i) {
        const PatternFrame picture = make(i);
        EXPECT_TRUE(renderer.submit(picture.frame));
        if (i > 0) {
            linuxos::GlVideoRenderer::Output output;
            output.choice = between(before, renderer.newestId(), 1, 2);
            surface.begin(gl);
            renderer.draw(surface.width, surface.height, output);
            EXPECT_TRUE(renderer.lastDrawWasGenerated());
            if (i == count - 1) computed = surface.read(gl);
        }
        before = renderer.newestId();
    }
    return computed;
}

std::vector<int> drawnAlone(const GlFunctions& gl, Surface& surface, const media::VideoFrame& frame) {
    linuxos::GlVideoRenderer reference;
    std::string error;
    EXPECT_TRUE(reference.create(GlWindow::get().loader(), error)) << error;
    EXPECT_TRUE(reference.submit(frame));
    surface.begin(gl);
    reference.draw(surface.width, surface.height);
    return surface.read(gl);
}

void checkStandingDisplay(const GlFunctions& gl, const int kPictures) {
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    constexpr double kStepX = 4.0;
    constexpr double kStepY = 1.0;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    OverlayFrame expected(kWidth, kHeight, kStepX * (kPictures - 1.5), kStepY * (kPictures - 1.5));
    const std::vector<int> wanted = drawnAlone(gl, surface, expected.picture.frame);
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
        VELOPLAY_MAKE_RENDERER(renderer);
        renderer.holdStillParts(hold == 1);
        const auto computed = runSequence(gl, surface, renderer, kPictures, [&](int i) {
            return OverlayFrame(kWidth, kHeight, kStepX * i, kStepY * i).picture;
        });
        displayError[hold] = offBy(computed, true);
        sceneError[hold] = offBy(computed, false);
    }
    std::printf("standing display over a moving scene, %d pictures: display off by %.2f grey levels without care and "
                "%.2f with it; scene off by %.2f and %.2f\n",
                kPictures, displayError[0], displayError[1], sceneError[0], sceneError[1]);
    EXPECT_LT(displayError[1], 1.5);
    EXPECT_LE(displayError[1], displayError[0] + 0.1);
    EXPECT_LT(sceneError[1], sceneError[0] + 0.3);
    EXPECT_LT(sceneError[1], 2.5);
    surface.destroy(gl);
}

} // namespace

TEST(GlFrameGeneration, PutsMovingContentWhereItBelongsInBetween) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    VELOPLAY_MAKE_RENDERER(renderer);
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    EXPECT_STREQ(renderer.frameGeneratorName(), "VeloPlay Flow");

    struct Case {
        double dx, dy;
        int step, steps;
    };
    for (const Case& test : {Case{8, -4, 1, 2}, Case{20, 6, 1, 2}, Case{40, -10, 1, 2}, Case{12, 0, 1, 4},
                             Case{12, 0, 3, 4}, Case{0, 0, 1, 2}}) {
        SCOPED_TRACE("moved by " + std::to_string(test.dx) + ", " + std::to_string(test.dy) + ", step " +
                     std::to_string(test.step) + " of " + std::to_string(test.steps));
        const double phase = static_cast<double>(test.step) / test.steps;
        renderer.clear();
        PatternFrame expected(kWidth, kHeight, test.dx * phase, test.dy * phase);
        ASSERT_TRUE(renderer.submit(expected.frame));
        surface.begin(gl);
        renderer.draw(kWidth, kHeight);
        const std::vector<int> wanted = surface.read(gl);
        renderer.clear();

        PatternFrame first(kWidth, kHeight, 0, 0);
        PatternFrame second(kWidth, kHeight, test.dx, test.dy);
        ASSERT_TRUE(renderer.submit(first.frame));
        const std::uint64_t firstId = renderer.newestId();
        ASSERT_TRUE(renderer.submit(second.frame));
        const std::uint64_t secondId = renderer.newestId();

        linuxos::GlVideoRenderer::Output output;
        output.choice.valid = true;
        output.choice.base = firstId;
        surface.begin(gl);
        renderer.draw(kWidth, kHeight, output);
        EXPECT_FALSE(renderer.lastDrawWasGenerated());
        const std::vector<int> a = surface.read(gl);
        output.choice.base = secondId;
        surface.begin(gl);
        renderer.draw(kWidth, kHeight, output);
        const std::vector<int> b = surface.read(gl);
        output.choice = between(firstId, secondId, test.step, test.steps);
        surface.begin(gl);
        renderer.draw(kWidth, kHeight, output);
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        expectCleanState(gl, surface.framebuffer, "a computed picture");
        const std::vector<int> computed = surface.read(gl);

        std::vector<int> mixed(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) mixed[i] = static_cast<int>(std::lround(a[i] * (1.0 - phase) + b[i] * phase));
        const double error = meanDifference(computed, wanted, kWidth, kHeight, 24);
        const double mixError = meanDifference(mixed, wanted, kWidth, kHeight, 24);
        std::printf("frame generation: moved %.0f,%.0f step %d/%d: off by %.2f grey levels, a plain mix by %.2f\n",
                    test.dx, test.dy, test.step, test.steps, error, mixError);
        EXPECT_LT(error, 2.5);
        if (test.dx != 0 || test.dy != 0) {
            EXPECT_LT(error, mixError * 0.4);
        }
    }
    surface.destroy(gl);
}

// A 60 fps game shown as it comes asks for no picture in between; then nothing is prepared for one.
TEST(GlFrameGeneration, PreparesNothingWhileNoPictureInBetweenIsWanted) {
    VELOPLAY_NEED_GL();
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kTarget, kTarget));
    VELOPLAY_MAKE_RENDERER(renderer);
    renderer.wantGeneration(false);
    SolidFrame dark(64, 64, 60, 128, 128, false);
    SolidFrame light(64, 64, 200, 128, 128, false);
    ASSERT_TRUE(renderer.submit(dark.frame));
    const std::uint64_t darkId = renderer.newestId();
    ASSERT_TRUE(renderer.submit(light.frame));
    linuxos::GlVideoRenderer::Output output;
    output.choice = between(darkId, renderer.newestId(), 1, 2);
    surface.begin(gl);
    renderer.draw(kTarget, kTarget, output);
    EXPECT_FALSE(renderer.lastDrawWasGenerated()) << "no picture of the pair was prepared";
    EXPECT_LT(surface.pixel(gl, 32, 32)[0], 120) << "the earlier real picture instead";
    surface.destroy(gl);
}

TEST(GlFrameGeneration, ShowsTheNewPageWhenAMenuSwitches) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    PageSwitch result[2];
    for (int care = 0; care < 2; ++care) {
        VELOPLAY_MAKE_RENDERER(renderer);
        ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
        renderer.holdStillParts(care == 1);
        result[care] = switchMenuPage(gl, surface, renderer, kWidth, kHeight);
    }
    std::printf("menu page switch: pages %.1f grey levels apart in the panel; computed picture %.2f from the new page "
                "without care, %.2f with it; around the panel off by %.2f and %.2f\n",
                result[1].panelApart, result[0].panelFromNew, result[1].panelFromNew, result[0].restFromOld,
                result[1].restFromOld);
    EXPECT_GT(result[1].panelApart, 10.0);
    EXPECT_LT(result[1].panelFromNew, 3.0);
    EXPECT_LT(result[1].restFromOld, 0.5);
    surface.destroy(gl);
}

TEST(GlFrameGeneration, DoesNotTearWhatMovesAcrossAStandingScene) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    constexpr int kPictures = 16;
    constexpr double kStep = 6.0;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    PassingFrame expected(kWidth, kHeight, 40.0 + kStep * (kPictures - 1.5));
    const std::vector<int> wanted = drawnAlone(gl, surface, expected.picture.frame);
    double off[2] = {0.0, 0.0};
    for (int care = 0; care < 2; ++care) {
        VELOPLAY_MAKE_RENDERER(renderer);
        renderer.holdStillParts(care == 1);
        const auto computed = runSequence(gl, surface, renderer, kPictures, [&](int i) {
            return PassingFrame(kWidth, kHeight, 40.0 + kStep * i).picture;
        });
        off[care] = meanDifference(computed, wanted, kWidth, kHeight, 0);
    }
    std::printf("a patch crossing a standing scene: off by %.2f grey levels without care, %.2f with it\n", off[0], off[1]);
    EXPECT_LT(off[1], off[0] + 0.15);
    EXPECT_LT(off[1], 2.0);
    surface.destroy(gl);
}

TEST(GlFrameGeneration, MovesColourThatBrightnessDoesNotShow) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    VELOPLAY_MAKE_RENDERER(renderer);
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    renderer.holdStillParts(false);
    for (const bool hand : {false, true}) {
        TintedShapeFrame first(kWidth, kHeight, 0.0, 0.0, 11, hand);
        TintedShapeFrame second(kWidth, kHeight, 8.0, 4.0, 12, hand);
        TintedShapeFrame expected(kWidth, kHeight, 4.0, 2.0, 13, hand);
        const Generated own = generateBetween(gl, surface, renderer, first.picture.frame, second.picture.frame,
                                              expected.picture.frame, kWidth, kHeight, 1);
        std::printf("blue shape as bright as the grey behind it%s: colour off by %.3f, by %.3f in a plain mix\n",
                    hand ? ", held by a hand" : "", own.tintError, own.tintMixError);
        EXPECT_LT(own.tintError, own.tintMixError * 0.6) << (hand ? "with hand" : "alone");
    }
    surface.destroy(gl);
}

TEST(GlFrameGeneration, ShowsARealPictureAcrossACut) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    VELOPLAY_MAKE_RENDERER(renderer);
    PatternFrame first(kWidth, kHeight, 0, 0);
    PatternFrame second(kWidth, kHeight, 5003, 2999);
    const auto show = [&](const media::VideoFrame& frame) {
        renderer.clear();
        EXPECT_TRUE(renderer.submit(frame));
        surface.begin(gl);
        renderer.draw(kWidth, kHeight);
        return surface.read(gl);
    };
    const std::vector<int> a = show(first.frame);
    const std::vector<int> b = show(second.frame);
    renderer.clear();
    ASSERT_TRUE(renderer.submit(first.frame));
    const std::uint64_t firstId = renderer.newestId();
    ASSERT_TRUE(renderer.submit(second.frame));
    const auto computedAt = [&](int step, int steps) {
        linuxos::GlVideoRenderer::Output output;
        output.choice = between(firstId, renderer.newestId(), step, steps);
        surface.begin(gl);
        renderer.draw(kWidth, kHeight, output);
        EXPECT_TRUE(renderer.lastDrawWasGenerated());
        return surface.read(gl);
    };
    const std::vector<int> early = computedAt(1, 4);
    const std::vector<int> late = computedAt(3, 4);
    const double earlyFromFirst = meanDifference(early, a, kWidth, kHeight, 0);
    const double lateFromSecond = meanDifference(late, b, kWidth, kHeight, 0);
    const double apart = meanDifference(a, b, kWidth, kHeight, 0);
    std::printf("cut: the two scenes are %.1f grey levels apart; a quarter in is %.2f from the first, three quarters "
                "in %.2f from the second\n",
                apart, earlyFromFirst, lateFromSecond);
    EXPECT_GT(apart, 20.0);
    EXPECT_LT(earlyFromFirst, 1.0);
    EXPECT_LT(lateFromSecond, 1.0);
    surface.destroy(gl);
}

TEST(GlFrameGeneration, KeepsWhatStandsStillWhereItIs) {
    VELOPLAY_NEED_GL();
    checkStandingDisplay(gl, 44); // a second and a half at 30 pictures a second
}

TEST(GlFrameGeneration, KeepsWhatAppearedHalfASecondAgoWhereItIs) {
    VELOPLAY_NEED_GL();
    checkStandingDisplay(gl, 16);
}

TEST(GlFrameGeneration, DoesNotHoldALadderThatKeepsMoving) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 256;
    constexpr int kHeight = 144;
    constexpr int kPictures = 24;
    constexpr double kStepY = 6.0;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    std::vector<int> computed[2];
    for (int hold = 0; hold < 2; ++hold) {
        VELOPLAY_MAKE_RENDERER(renderer);
        renderer.holdStillParts(hold == 1);
        computed[hold] = runSequence(gl, surface, renderer, kPictures, [&](int i) {
            return LadderFrame(kWidth, kHeight, 0.0, kStepY * i).picture;
        });
    }
    const int left = static_cast<int>(kWidth * 0.35);
    const int right = static_cast<int>(kWidth * 0.65);
    double rungs = 0.0, rims = 0.0, scene = 0.0;
    int rungCount = 0, rimCount = 0, sceneCount = 0;
    for (int y = 8; y < kHeight - 8; ++y) {
        for (int x = 16; x < kWidth - 16; ++x) {
            const std::size_t at = static_cast<std::size_t>(y * kWidth + x);
            const double difference = std::abs(computed[1][at] - computed[0][at]);
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
    surface.destroy(gl);
}

// The scenes of OnTheGraphicsCard.FindsMotionWithNvidiasEngineWhereThereIsOne, with the built-in search,
// at the same size. On the RTX 4070 the built-in search came to 0.00 for both textured pictures, about 30
// for the ladder with the camera moving down (a plain mix 46.5: the one scene where a search from coarse to
// fine goes wrong) and 0.6 to 0.7 sideways; blue on grey about 0.19. The time here is llvmpipe on the
// processor, not a graphics card.
TEST(GlFrameGeneration, MeasuresTheSearchOnTheScenesOfTheGraphicsCardTest) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    VELOPLAY_MAKE_RENDERER(renderer);
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    renderer.holdStillParts(false); // the same pair again and again would look like a picture that never changes
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
    for (const Scene& scene : scenes) {
        const auto frame = [&](double share) {
            return scene.ladder ? LadderFrame(kWidth, kHeight, scene.dx * share, scene.dy * share).picture
                                : PatternFrame(kWidth, kHeight, scene.dx * share, scene.dy * share);
        };
        const PatternFrame first = frame(0.0);
        const PatternFrame second = frame(1.0);
        const PatternFrame expected = frame(0.5);
        const Generated own = generateBetween(gl, surface, renderer, first.frame, second.frame, expected.frame, kWidth,
                                              kHeight, 3);
        std::printf("motion search at 640 x 360, %s: built-in off by %.2f grey levels in %.2f ms, a plain mix off by "
                    "%.2f\n",
                    scene.name, own.error, own.milliseconds, own.mixError);
        if (!scene.ladder) {
            EXPECT_LT(own.error, own.mixError * 0.6) << scene.name;
        }
    }
    for (const bool hand : {false, true}) {
        TintedShapeFrame first(kWidth, kHeight, 0.0, 0.0, 11, hand);
        TintedShapeFrame second(kWidth, kHeight, 10.0, 4.0, 12, hand);
        TintedShapeFrame expected(kWidth, kHeight, 5.0, 2.0, 13, hand);
        const Generated own = generateBetween(gl, surface, renderer, first.picture.frame, second.picture.frame,
                                              expected.picture.frame, kWidth, kHeight, 1);
        std::printf("motion search at 640 x 360, blue on grey%s: built-in %.3f, plain mix %.3f\n",
                    hand ? " with hand" : "", own.tintError, own.tintMixError);
        EXPECT_LT(own.tintError, own.tintMixError * 0.6) << (hand ? "with hand" : "alone");
    }
    // The menu page at this size, with care (on the RTX 4070: 0.38 for the built-in search).
    VELOPLAY_MAKE_RENDERER(menu);
    const PageSwitch page = switchMenuPage(gl, surface, menu, kWidth, kHeight);
    std::printf("motion search at 640 x 360, menu page: apart %.1f, from new page %.2f with care, rest %.2f\n",
                page.panelApart, page.panelFromNew, page.restFromOld);
    EXPECT_LT(page.panelFromNew, 3.0);
    surface.destroy(gl);
}

TEST(GlFrameGeneration, SaysHowLongAPictureTakesAtFullSize) {
    VELOPLAY_NEED_GL();
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    Surface surface;
    ASSERT_TRUE(surface.create(gl, kWidth, kHeight));
    VELOPLAY_MAKE_RENDERER(renderer);
    ASSERT_TRUE(renderer.frameGenerationAvailable()) << renderer.frameGeneratorError();
    renderer.holdStillParts(false);
    PatternFrame first(kWidth, kHeight, 0, 0);
    PatternFrame second(kWidth, kHeight, 18, -6);
    PatternFrame expected(kWidth, kHeight, 9, -3);
    const Generated own = generateBetween(gl, surface, renderer, first.frame, second.frame, expected.frame, kWidth,
                                          kHeight, 2);
    // The same pair shown as it comes, for what upload and drawing alone cost.
    double plain = 1e9;
    for (int round = 0; round < 2; ++round) {
        renderer.clear();
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(renderer.submit(second.frame));
        surface.begin(gl);
        renderer.draw(kWidth, kHeight);
        (void)surface.read(gl);
        plain = std::min(plain, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("motion search at 1920 x 1080: %.2f ms per computed picture (off by %.2f, a plain mix %.2f); a real "
                "picture uploaded and shown alone %.2f ms; on %s\n",
                own.milliseconds, own.error, own.mixError, plain, reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)));
    EXPECT_LT(own.error, own.mixError * 0.6);
    surface.destroy(gl);
}
