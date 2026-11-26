#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "app/UiModel.hpp"
#include "media/VideoFrame.hpp"
#include "session/ISession.hpp"
#include "util/Result.hpp"

struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;
struct SDL_AudioStream;

namespace veloplay::linuxos {

/// The video capture devices under `folder` (capture cards, webcams): the
/// nodes /dev/videoN that deliver pictures, without the metadata nodes a
/// webcam brings beside them. Each is opened for a moment to ask what it is,
/// so call it away from the window thread. `why` says why the list is empty
/// when that is not simply "nothing plugged in" (no permission, say).
std::vector<CaptureDevice> listCaptureDevices(std::string* why = nullptr,
                                              const std::filesystem::path& folder = "/dev");

/// One picture format a device offers: a V4L2 pixel format, a size and the
/// most pictures per second it gives at that size.
struct CaptureMode {
    std::uint32_t format = 0; // V4L2_PIX_FMT_...
    int width = 0;
    int height = 0;
    double fps = 0.0;
};

/// Whether this program reads that V4L2 pixel format: NV12, YUYV, I420 and MJPEG.
[[nodiscard]] bool readsCaptureFormat(std::uint32_t format);

/// The best of what a device offers for this program: up to 1080p (a larger
/// picture only if nothing else is there), then as many pictures as possible
/// up to 60, then uncompressed before compressed (a decoder adds a little
/// delay), NV12 first among those (nothing to convert). Nothing if no format is one it reads.
[[nodiscard]] std::optional<CaptureMode> chooseCaptureMode(const std::vector<CaptureMode>& offered);

/// The colours of a raw picture as V4L2 names them, with its rules for "default".
/// A driver that says nothing at all (all three 0) gets what HDMI sends at
/// that size: BT.709 from 720 lines up, like Windows assumes for a card that says nothing.
[[nodiscard]] media::ColorInfo captureColor(std::uint32_t colorspace, std::uint32_t ycbcrEncoding,
                                            std::uint32_t quantization, int height);

/// YUYV (4:2:2, Y U Y V per two pixels) to NV12 (4:2:0): the colour of two
/// rows is averaged into one. `width` must be even.
void yuyvToNv12(const std::uint8_t* yuyv, int yuyvStride, int width, int height, std::uint8_t* luma, int lumaStride,
                std::uint8_t* chroma, int chromaStride);

/// Turns MJPEG pictures, as most USB capture cards send 1080p at 60, into
/// pictures for the pipeline: 4:2:0 as I420 straight from the decoder, 4:2:2
/// and others as NV12 through swscale, in the range the JPEG has.
class MjpegDecoder {
public:
    MjpegDecoder() = default;
    MjpegDecoder(const MjpegDecoder&) = delete;
    MjpegDecoder& operator=(const MjpegDecoder&) = delete;
    ~MjpegDecoder();

    Result<media::VideoFrame> decode(std::span<const std::uint8_t> jpeg);

private:
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* scale_ = nullptr;
};

/// Reads pictures from a V4L2 device (memory-mapped buffers) on a thread of its
/// own and sound from the sound input of the same card through SDL, and hands
/// both on as they are: pictures in CPU memory as NV12 or I420, sound as 16-bit
/// samples at 48 kHz stereo. The sound needs SDL's audio started on the main
/// thread beforehand; without it the picture runs alone.
class V4l2Source {
public:
    V4l2Source() = default;
    V4l2Source(const V4l2Source&) = delete;
    V4l2Source& operator=(const V4l2Source&) = delete;
    ~V4l2Source();

    /// A capture card by its path from listCaptureDevices(), and the sound input that belongs to `name`, if any.
    Result<void> openDevice(const std::string& path, const std::string& name);
    /// Starts handing on to `sink` from threads of their own.
    void start(session::IMediaSink* sink);
    /// Ends the reading, also one that waits for a picture; nothing reaches the sink after it returns.
    void stop();
    /// From now on nothing is handed on. Returns at once.
    void detachSink() { sink_.store(nullptr); }

    [[nodiscard]] std::uint64_t pictures() const { return pictures_.load(); }
    [[nodiscard]] bool ended() const { return ended_.load(); }
    [[nodiscard]] std::string failure();
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] bool sound() const { return sound_ != nullptr; }

private:
    struct Mapped {
        void* data = nullptr;
        std::size_t length = 0;
    };
    Result<void> setUpVideo();
    void openSound(const std::string& name);
    void run();
    void handOn(const std::uint8_t* data, std::size_t length);
    void close();
    static void soundArrived(void* self, SDL_AudioStream* stream, int additional, int total);

    int fd_ = -1;
    int wakeFd_ = -1; // an eventfd: written to end a poll() that waits for a picture
    std::string path_;
    std::vector<Mapped> buffers_;
    bool streaming_ = false;
    std::uint32_t format_ = 0;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    std::size_t imageSize_ = 0;
    media::ColorInfo color_{};
    MjpegDecoder mjpeg_;
    std::size_t undecodable_ = 0;

    SDL_AudioStream* sound_ = nullptr;
    std::vector<std::int16_t> soundBuffer_; // only touched in soundArrived, under SDL's lock of the stream

    std::atomic<session::IMediaSink*> sink_{nullptr};
    std::thread thread_;
    std::atomic<bool> ended_{false};
    std::atomic<std::uint64_t> pictures_{0};
    std::mutex mutex_;
    std::string failure_;
};

/// A capture card behind the session interface, so the rest of the program
/// shows it like a stream from a console. There is nothing to send back:
/// input, the login passcode and rest mode do nothing. `config.host` is the
/// device's path, `config.name` its name (to find its sound input by).
class CaptureSession final : public session::ISession {
public:
    CaptureSession() = default;
    ~CaptureSession() override;

    Result<void> start(const session::SessionConfig& config, session::IMediaSink* sink) override;
    /// Returns at once: the reading is ended on a thread of its own.
    void stop() override;
    /// Always: the program then lets go of the session on another thread instead of waiting for it.
    [[nodiscard]] bool slowToEnd() const override { return true; }
    void detachSink() override { source_.detachSink(); }
    std::vector<session::SessionEvent> pollEvents() override;
    void setControllerState(const input::ControllerState&, session::Clock::time_point) override {}
    Result<void> submitLoginPin(std::string_view) override {
        return Error{ErrorCode::InvalidArgument, "a capture card has no passcode"};
    }
    Result<void> sleepConsole() override {
        return Error{ErrorCode::InvalidArgument, "a capture card cannot put a console to rest"};
    }
    session::SessionStats stats(session::Clock::time_point now) override;

private:
    V4l2Source source_;
    std::thread opener_; // opening a device takes a moment; the window does not wait for it
    std::thread stopper_; // nor for its end
    std::mutex mutex_;
    std::string openFailure_;
    bool opened_ = false;
    bool announced_ = false;
    bool quitSent_ = false;
    std::atomic<bool> stopped_{false};
    std::uint64_t countedPictures_ = 0;
    session::Clock::time_point countedAt_{};
    double fps_ = 0.0;
};

/// For AppController::Options::captureFactory.
session::SessionFactory captureSessionFactory();

} // namespace veloplay::linuxos
