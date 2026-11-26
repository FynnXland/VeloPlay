#include "linux/GlCapture.hpp"

#include <algorithm>
#include <cstring>

namespace veloplay::linuxos {

namespace {

/// Starts the copy of a region into `buffer`, which is made or grown to fit. OpenGL counts rows from the
/// bottom, so the region is turned over; the rows arrive bottom first and are turned back when read.
GLsync startCopy(const GlFunctions& gl, GLuint& buffer, int left, int top, int width, int height, int framebufferHeight,
                 bool resize) {
    const GLsizeiptr size = static_cast<GLsizeiptr>(width) * height * 4;
    if (!buffer) {
        gl.GenBuffers(1, &buffer);
        resize = true;
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    if (resize) gl.BufferData(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    // Blue, green, red, alpha comes straight out of OpenGL: no swapping afterwards.
    gl.ReadPixels(left, framebufferHeight - top - height, width, height, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    GLsync fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.Flush(); // so the card starts on it now and not at the swap
    return fence;
}

/// Whether the card is done with what came before `fence`, without waiting for it.
bool arrived(const GlFunctions& gl, GLsync fence, bool& failed) {
    const GLenum state = gl.ClientWaitSync(fence, 0, 0);
    failed = state == GL_WAIT_FAILED;
    return state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED;
}

/// Copies the rows out of `buffer`, top row first.
bool readRows(const GlFunctions& gl, GLuint buffer, int width, int height, std::vector<std::uint8_t>& out) {
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    const std::size_t size = row * static_cast<std::size_t>(height);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    const auto* data = static_cast<const std::uint8_t*>(
        gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT));
    if (!data) {
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }
    out.resize(size);
    for (int y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * row,
                    data + static_cast<std::size_t>(height - 1 - y) * row, row);
    }
    gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

} // namespace

GlFrameGrabber::~GlFrameGrabber() { release(); }

void GlFrameGrabber::release() {
    if (gl_ && gl_->loaded()) {
        if (fence_) gl_->DeleteSync(fence_);
        if (buffer_) gl_->DeleteBuffers(1, &buffer_);
    }
    abandon();
}

void GlFrameGrabber::abandon() {
    fence_ = nullptr;
    buffer_ = 0;
    pending_ = false;
    width_ = 0;
    height_ = 0;
}

void GlFrameGrabber::capture(int left, int top, int width, int height, int framebufferHeight) {
    if (!wanted_ || pending_) return;
    wanted_ = false;
    failure_ = "";
    if (!gl_ || !gl_->loaded()) {
        failure_ = "the picture cannot be read without OpenGL";
        return;
    }
    left = std::max(0, left);
    top = std::max(0, top);
    height = std::min(height, framebufferHeight - top);
    if (width <= 0 || height <= 0) {
        failure_ = "there is no picture on screen";
        return;
    }
    const bool resize = width != width_ || height != height_;
    fence_ = startCopy(*gl_, buffer_, left, top, width, height, framebufferHeight, resize);
    if (!fence_) {
        failure_ = "no memory for a copy of the picture";
        return;
    }
    width_ = width;
    height_ = height;
    pending_ = true;
    waited_ = 0;
}

std::optional<media::Image> GlFrameGrabber::take() {
    if (!pending_ || !gl_ || !gl_->loaded()) return std::nullopt;
    bool failed = false;
    if (!arrived(*gl_, fence_, failed) && !failed) {
        // A card that has not got there after a few hundred frames will not; give the request up.
        if (++waited_ > 600) {
            pending_ = false;
            gl_->DeleteSync(fence_);
            fence_ = nullptr;
            failure_ = "the graphics card did not hand the picture over";
        }
        return std::nullopt;
    }
    pending_ = false;
    gl_->DeleteSync(fence_);
    fence_ = nullptr;
    media::Image image;
    if (failed || !readRows(*gl_, buffer_, width_, height_, image.bgra)) {
        failure_ = "the copy of the picture could not be read";
        return std::nullopt;
    }
    image.width = width_;
    image.height = height_;
    // The screen has no transparency; what the framebuffer holds there is meaningless.
    for (std::size_t at = 3; at < image.bgra.size(); at += 4) image.bgra[at] = 255;
    return image;
}

GlFrameStream::~GlFrameStream() { reset(); }

void GlFrameStream::reset() {
    if (gl_ && gl_->loaded()) {
        for (Slot& slot : slots_) {
            if (slot.fence) gl_->DeleteSync(slot.fence);
            if (slot.buffer) gl_->DeleteBuffers(1, &slot.buffer);
        }
    }
    abandon();
}

void GlFrameStream::abandon() {
    for (Slot& slot : slots_) slot = Slot{};
    next_ = 0;
    oldest_ = 0;
    width_ = 0;
    height_ = 0;
}

bool GlFrameStream::capture(int left, int top, int width, int height, int framebufferHeight, media::ShownKind kind,
                            std::chrono::steady_clock::time_point at) {
    if (!gl_ || !gl_->loaded()) {
        failure_ = "the picture cannot be recorded without OpenGL";
        return false;
    }
    if (width <= 0 || height <= 0 || left < 0 || top < 0 || top + height > framebufferHeight) return false;
    if (width != width_ || height != height_) {
        reset();
        width_ = width;
        height_ = height;
    }
    Slot& slot = slots_[next_];
    if (slot.pending) return false;
    slot.fence = startCopy(*gl_, slot.buffer, left, top, width, height, framebufferHeight, slot.buffer == 0);
    if (!slot.fence) {
        failure_ = "no memory for copies of the picture";
        return false;
    }
    slot.pending = true;
    slot.kind = kind;
    slot.at = at;
    next_ = (next_ + 1) % slots_.size();
    return true;
}

std::optional<GlFrameStream::Picture> GlFrameStream::take(std::vector<std::uint8_t> reuse) {
    Slot& slot = slots_[oldest_];
    if (!slot.pending || !gl_ || !gl_->loaded()) return std::nullopt;
    bool failed = false;
    if (!arrived(*gl_, slot.fence, failed) && !failed) return std::nullopt;
    slot.pending = false;
    gl_->DeleteSync(slot.fence);
    slot.fence = nullptr;
    oldest_ = (oldest_ + 1) % slots_.size();
    Picture picture;
    picture.kind = slot.kind;
    picture.at = slot.at;
    picture.bgra = std::move(reuse);
    if (failed || !readRows(*gl_, slot.buffer, width_, height_, picture.bgra)) {
        failure_ = "a copy of the picture could not be read";
        return std::nullopt;
    }
    return picture;
}

} // namespace veloplay::linuxos
