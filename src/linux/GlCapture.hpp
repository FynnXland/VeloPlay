#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "linux/GlFunctions.hpp"
#include "media/Recording.hpp"
#include "media/Snapshot.hpp"

namespace veloplay::linuxos {

/// Takes a copy of the picture as it is on screen, after frame generation and
/// scaling, before any of VeloPlay's own overlays are drawn over it: the
/// counterpart of render::FrameGrabber.
///
/// Never waits for the graphics card: capture() only asks the card to copy
/// the region into a buffer of its own and sets a fence; take() looks once
/// per frame whether the fence has passed. A screenshot arrives one or two
/// frames after the key, and the window keeps its rhythm.
///
/// Reads from the framebuffer bound for reading (normally 0, whose back
/// buffer holds the picture until the swap) and leaves that binding alone.
/// The context must be current for every call; it must not be multisampled.
class GlFrameGrabber {
public:
    /// `gl` are the functions of the context, for example GlVideoRenderer::gl(). They must outlive this.
    explicit GlFrameGrabber(const GlFunctions* gl = nullptr) : gl_(gl) {}
    ~GlFrameGrabber();
    GlFrameGrabber(const GlFrameGrabber&) = delete;
    GlFrameGrabber& operator=(const GlFrameGrabber&) = delete;
    void setFunctions(const GlFunctions* gl) { gl_ = gl; }

    /// The next capture() takes a copy.
    void request() { wanted_ = true; }
    [[nodiscard]] bool busy() const { return wanted_ || pending_; }

    /// Call right after the picture was drawn. The region is the part that shows the picture, without the
    /// black bars, in pixels from the top left of the framebuffer, which is `framebufferHeight` high. Does
    /// nothing unless a copy was requested.
    void capture(int left, int top, int width, int height, int framebufferHeight);

    /// The picture once its copy is there, rows top to bottom in blue, green, red, alpha; nothing while it
    /// is on its way.
    std::optional<media::Image> take();

    /// Why the last request came to nothing, empty if it did not.
    [[nodiscard]] const char* failure() const { return failure_; }

    /// Frees the buffer (the context must be current). abandon() forgets it for a context that is gone.
    void release();
    void abandon();

private:
    const GlFunctions* gl_ = nullptr;
    GLuint buffer_ = 0;
    GLsync fence_ = nullptr;
    bool wanted_ = false;
    bool pending_ = false;
    int width_ = 0;
    int height_ = 0;
    int waited_ = 0;
    const char* failure_ = "";
};

/// The same for a recording: a copy of every picture, in order. The copies
/// go through a ring of four buffers the card fills on its own time, so it
/// has a few frames to hand each one over and the window never waits for it.
class GlFrameStream {
public:
    struct Picture {
        std::vector<std::uint8_t> bgra; // rows top to bottom, four bytes per pixel
        media::ShownKind kind = media::ShownKind::Real;
        std::chrono::steady_clock::time_point at;
    };

    explicit GlFrameStream(const GlFunctions* gl = nullptr) : gl_(gl) {}
    ~GlFrameStream();
    GlFrameStream(const GlFrameStream&) = delete;
    GlFrameStream& operator=(const GlFrameStream&) = delete;
    void setFunctions(const GlFunctions* gl) { gl_ = gl; }

    /// Queues a copy of `width` x `height` from `left, top` (from the top left of the framebuffer, which is
    /// `framebufferHeight` high). False if the ring is full (the copies before have not arrived yet), so
    /// this picture is left out.
    bool capture(int left, int top, int width, int height, int framebufferHeight, media::ShownKind kind,
                 std::chrono::steady_clock::time_point at);
    /// The oldest copy once it has arrived. `reuse` is memory for it, may be empty.
    std::optional<Picture> take(std::vector<std::uint8_t> reuse = {});
    /// Forgets the copies on their way and frees the buffers (the context must be current).
    void reset();
    /// Forgets everything for a context that is gone.
    void abandon();
    [[nodiscard]] const char* failure() const { return failure_; }

private:
    struct Slot {
        GLuint buffer = 0;
        GLsync fence = nullptr;
        bool pending = false;
        media::ShownKind kind = media::ShownKind::Real;
        std::chrono::steady_clock::time_point at;
    };
    const GlFunctions* gl_ = nullptr;
    std::array<Slot, 4> slots_;
    std::size_t next_ = 0;   // where the next copy goes
    std::size_t oldest_ = 0; // the copy to hand over next
    int width_ = 0;
    int height_ = 0;
    const char* failure_ = "";
};

} // namespace veloplay::linuxos
