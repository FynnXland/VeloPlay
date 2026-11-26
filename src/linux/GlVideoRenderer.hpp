#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "linux/GlFrameGenerator.hpp"
#include "linux/GlFunctions.hpp"
#include "media/FramePacer.hpp"
#include "media/VideoFrame.hpp"

namespace veloplay::linuxos {

/// Where a program keeps a uniform, asked once. Asking the driver on every draw would make a driver that
/// runs on a thread of its own (Mesa's glthread) stop and wait for that thread each time. Names are compared
/// by address: they are the string literals of the renderer.
class UniformCache {
public:
    GLint at(const GlFunctions& gl, GLuint program, const char* name);
    void clear() { entries_.clear(); }

private:
    struct Entry {
        GLuint program;
        const char* name;
        GLint location;
    };
    std::vector<Entry> entries_;
};

/// Brings decoded pictures to the screen with OpenGL 3.3 core: the Linux
/// counterpart of render::VideoRenderer (Direct3D 11), with the same three
/// stages and the same shaders, written in GLSL:
///
/// 1. Each picture from the console is uploaded from processor memory and
///    converted from YUV to RGB once, into a texture of its own size. The
///    two newest are kept.
/// 2. If a picture between those two is asked for, the frame generator
///    computes it (VeloPlay's own flow method; NVIDIA's and AMD's engines do
///    not exist here).
/// 3. Banding is smoothed and a style applied if wanted, then the picture is
///    scaled to the window, softly or with a Lanczos filter, and sharpened.
///
/// Inside the stages a texture's first row is the top of the picture, as in
/// Direct3D, so the shaders and every shift they compute read the same. The
/// framebuffer draw() paints into is taken the OpenGL way, first row at the
/// bottom of the screen, and only that last pass turns the picture over.
///
/// Every call needs the context create() was given to be current. The
/// destructor and reset() free what was made in it; after the context is
/// gone, call abandon() first.
class GlVideoRenderer {
public:
    /// What draw() is to show and how.
    struct Output {
        media::FrameChoice choice; // which picture; not valid means the newest
        bool upscale = false;      // Lanczos instead of soft enlargement
        float sharpness = 0.0f;    // 0 = leave as it is, 1 = strongest
        bool deband = false;       // smooth the steps a low bitrate leaves in flat gradients
        int look = 0;              // picture style: 0 as it comes, 1 vivid, 2 cinematic
        float lookStrength = 1.0f; // 0 = as it comes, 1 = the full style
    };

    /// How much of a picture differs from the one before it.
    struct Change {
        std::uint64_t picture = 0; // counted like submitted()
        double share = 0.0;        // part of the picture that changed visibly; 0 = none of it, 1 = all
        double mean = 0.0;         // mean difference in brightness levels, 0 to 255
    };

    GlVideoRenderer() = default;
    ~GlVideoRenderer();
    GlVideoRenderer(const GlVideoRenderer&) = delete;
    GlVideoRenderer& operator=(const GlVideoRenderer&) = delete;

    /// Needs a current OpenGL 3.3 core context. Loads the functions through `load` and compiles the shaders.
    bool create(GlProcLoader load, std::string& error);
    /// The functions create() loaded, for the parts that work next to the renderer (GlCapture).
    [[nodiscard]] const GlFunctions& gl() const { return gl_; }

    /// Takes over a picture in processor memory (PixelLayout::I420 or Nv12). Returns false if it could not
    /// be brought onto the card.
    ///
    /// Every picture is also compared with the one before it, coarsely; the
    /// results come out of takeChange() a few pictures later. With `keep`
    /// false that is all that happens: the picture is measured and not shown.
    /// That is for a game at 30 fps in a 60 fps stream, whose every other
    /// picture repeats the one before.
    bool submit(const media::VideoFrame& frame, bool keep = true);

    /// The next finished comparison, oldest first. Never waits for the card.
    std::optional<Change> takeChange();
    /// Pictures handed to submit() so far, kept or not.
    [[nodiscard]] std::uint64_t submitted() const { return submitted_; }

    [[nodiscard]] bool hasPicture() const { return hasPicture_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    /// Number of the picture taken over last; they count up from 1.
    [[nodiscard]] std::uint64_t newestId() const { return slots_[newest_].id; }

    /// Draws into the framebuffer bound for drawing when it is called, centred and with the aspect ratio
    /// kept. The area around the picture is left as it was cleared. `targetWidth` and `targetHeight` are
    /// that framebuffer's size in pixels.
    void draw(int targetWidth, int targetHeight, const Output& output);
    void draw(int targetWidth, int targetHeight) { draw(targetWidth, targetHeight, Output{}); }

    /// False if frame generation could not be set up; the reason is in frameGeneratorError(). Pictures are
    /// then shown as they arrive.
    [[nodiscard]] bool frameGenerationAvailable() const { return generator_ != nullptr; }
    [[nodiscard]] const char* frameGeneratorName() const { return generator_ ? generator_->name() : ""; }
    [[nodiscard]] const std::string& frameGeneratorError() const { return generatorError_; }
    /// Whether pictures in between will be asked for. While they are not, nothing is prepared for them:
    /// a 60 fps game shown as it is costs no motion search.
    void wantGeneration(bool wanted) { generationWanted_ = wanted; }
    /// Switches the care for parts that stand still off or on, to see what it does.
    void holdStillParts(bool hold) {
        if (generator_) generator_->holdStillParts(hold);
        generatedFor_ = {};
    }

    /// What the last draw() put the picture through on its way to the window: "Lanczos" or, for the
    /// card's plain filter, nothing.
    [[nodiscard]] const char* scalerName() const { return scalerName_; }
    /// Whether the last draw smoothed banding.
    [[nodiscard]] bool debanded() const { return debandUsed_; }
    /// True if the last draw() gave the picture a style.
    [[nodiscard]] bool styled() const { return styleUsed_; }
    /// Size of the picture on screen at the last draw(), in pixels; 0 before the first.
    [[nodiscard]] int shownWidth() const { return shownWidth_; }
    [[nodiscard]] int shownHeight() const { return shownHeight_; }
    /// Where the picture lay at the last draw(), from the top left of the framebuffer, in pixels. With the
    /// size above, that is the region for a screenshot or a recording.
    [[nodiscard]] int shownLeft() const { return shownLeft_; }
    [[nodiscard]] int shownTop() const { return shownTop_; }
    /// True if the last draw() showed a computed picture.
    [[nodiscard]] bool lastDrawWasGenerated() const { return lastGenerated_; }

    /// Forgets the pictures, for example when a session ends.
    void clear();
    /// Frees everything made in the context, as if create() had never run. The context must be current.
    void reset();
    /// Forgets everything made in the context without freeing it, for a context that is already gone.
    void abandon();

private:
    struct Slot {
        GlTarget rgb;
        std::uint64_t id = 0; // 0 = empty
    };
    struct Plane {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
    };
    // The comparison of each picture with the one before. A small copy of
    // its brightness is drawn and read back into a buffer the card fills on
    // its own time; a fence says when it is done. It is read a few pictures
    // later: reading at once would make the whole pipeline wait for the card.
    static constexpr int kProbeWidth = 96;
    static constexpr int kProbeHeight = 54;
    struct ProbeCopy {
        GLuint buffer = 0;
        GLsync fence = nullptr;
        std::uint64_t picture = 0;
        bool pending = false;
    };

    bool ensurePlanes(int width, int height, media::PixelLayout layout);
    bool upload(const media::VideoFrame& frame);
    void probe();
    void collect(const ProbeCopy* mustGo);
    void readProbe(ProbeCopy& copy);
    void dropProbe(ProbeCopy& copy);
    GLuint pictureFor(const media::FrameChoice& choice);
    void releaseAll();
    void restore(GLint drawFramebuffer, GLint readFramebuffer);

    GlFunctions gl_;
    UniformCache uniforms_;
    bool created_ = false;
    GLuint vertexArray_ = 0;
    GLuint zero_ = 0;
    GLuint convertProgram_ = 0;
    GLuint probeProgram_ = 0;
    GLuint softProgram_ = 0;
    GLuint lanczosProgram_ = 0;
    GLuint sharpenProgram_ = 0;
    GLuint debandProgram_ = 0;
    GLuint lookProgram_ = 0;
    GLuint uploadBuffer_ = 0; // the pixels go through it, so the copy to the card does not hold the window up

    Plane luma_;
    Plane chromaU_;  // I420: U; NV12: U and V side by side
    Plane chromaV_;  // I420 only
    media::PixelLayout layout_ = media::PixelLayout::I420;

    Slot slots_[2];  // the two newest pictures, as RGB
    int newest_ = 0; // which of the two is the newer one
    std::uint64_t nextId_ = 1;
    GlTarget generated_; // the picture computed last
    media::FrameChoice generatedFor_{};
    GlTarget scaled_;    // the picture at window size, when sharpening needs one
    GlTarget debanded_;  // the picture with its banding smoothed, at the stream's size
    bool debandUsed_ = false;
    GlTarget styled_;    // the picture in its style, at the stream's size
    bool styleUsed_ = false;
    std::unique_ptr<GlFrameGenerator> generator_;
    std::string generatorError_;
    bool generationWanted_ = true;
    bool prepared_[2] = {false, false}; // the generator has seen the picture in this slot
    const char* scalerName_ = "";
    bool lastGenerated_ = false;
    int shownWidth_ = 0;
    int shownHeight_ = 0;
    int shownLeft_ = 0;
    int shownTop_ = 0;

    GlTarget probeTarget_;
    ProbeCopy probeCopies_[3];
    std::vector<std::uint8_t> probeBefore_; // the small copy of the picture read last
    std::uint64_t probeBeforePicture_ = 0;
    std::deque<Change> changes_;
    std::uint64_t submitted_ = 0;

    int width_ = 0;
    int height_ = 0;
    bool hasPicture_ = false;
    media::ColorInfo color_{};
    bool colorSet_ = false;
};

} // namespace veloplay::linuxos
