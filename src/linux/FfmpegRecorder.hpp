#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "media/Recording.hpp"
#include "record/VideoRecorder.hpp" // RecordingSummary only; the class there is the one for Windows
#include "util/Result.hpp"

namespace veloplay::linuxos {

using record::RecordingSummary;

/// Writes the pictures on screen and the sound that is played into an MP4
/// file through FFmpeg: H.264 (or MPEG-4 Part 2 where this FFmpeg has no
/// H.264 encoder) and AAC. The same contract as record::VideoRecorder on
/// Windows: the writing runs on a thread of its own, the window and the sound
/// device hand over and go on, nothing here waits for the encoder in either.
class VideoRecorder {
public:
    VideoRecorder() = default;
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;
    ~VideoRecorder();

    /// Whether this FFmpeg can write a recording: an MP4 muxer, an AAC encoder
    /// and one of the picture encoders. `why` says why not, and stays empty when it can.
    static bool available(std::string* why = nullptr);
    /// The picture encoder start() tries first ("libx264", "libopenh264", "mpeg4"), for the log; empty if none.
    static std::string firstEncoder();

    /// Opens the file and the encoders; blocks for that long (tens of milliseconds).
    /// Width and height must be even, as H.264 wants them.
    Result<bool> start(const std::filesystem::path& file, int width, int height, int fps);
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    /// Seconds since the start, for the display.
    [[nodiscard]] double seconds() const;
    /// The FFmpeg encoder writing the picture ("libx264", "libopenh264", "mpeg4"); empty before start().
    [[nodiscard]] std::string encoder() const;
    /// From the next start() on, tries only this picture encoder. For tests of the fallbacks.
    void useOnlyEncoder(std::string name) { onlyEncoder_ = std::move(name); }

    /// A buffer for the next picture, from the ones already written if there are.
    std::vector<std::uint8_t> buffer();
    /// One picture, four bytes per pixel (blue, green, red, unused), rows top
    /// to bottom without gaps. False if it was left out because the writer is behind.
    bool addPicture(std::vector<std::uint8_t>&& bgra, media::ShownKind kind, std::chrono::steady_clock::time_point at);
    /// Sound as it goes to the speakers, from any thread. Anything but 48 kHz stereo is left out.
    void addSound(std::span<const std::int16_t> interleaved, int sampleRate, int channels);

    /// Writes what is left and closes the file so it plays. Safe to call twice.
    Result<RecordingSummary> stop();

private:
    struct Picture {
        std::vector<std::uint8_t> pixels;
        media::ShownKind kind = media::ShownKind::Real;
        std::chrono::steady_clock::time_point at;
    };
    struct Writer; // FFmpeg's state, owned by the writing thread
    void write(Writer& writer);

    std::atomic<bool> running_{false};
    std::thread thread_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    std::filesystem::path file_;
    std::chrono::steady_clock::time_point started_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::deque<Picture> pictures_;
    std::vector<std::int16_t> sound_;
    std::vector<std::vector<std::uint8_t>> spare_;
    std::size_t dropped_ = 0;
    bool soundArrived_ = false;
    std::string encoder_;
    std::string onlyEncoder_;
    std::string error_;
    RecordingSummary summary_;
};

} // namespace veloplay::linuxos
