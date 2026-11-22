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
#include "util/Result.hpp"

struct IMFSinkWriter;

namespace veloplay::record {

/// What a finished recording holds.
struct RecordingSummary {
    std::filesystem::path file;
    std::size_t pictures = 0; // written into the file
    std::size_t dropped = 0;  // came faster than they could be written and were left out
    double seconds = 0.0;
    bool sound = false; // any sound arrived; without, the file has silence
    media::FrameLog log;
};

/// Writes the pictures on screen and the sound that is played into an MP4
/// file, H.264 and AAC, through Media Foundation's sink writer. The writing
/// runs on a thread of its own: the window hands pictures over and goes on,
/// the sound device hands sound over and goes on. Nothing here waits for the
/// encoder in either of them.
class VideoRecorder {
public:
    VideoRecorder() = default;
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;
    ~VideoRecorder();

    /// Whether this PC can record at all; `why` says why not.
    static bool available(std::string* why = nullptr);

    /// Opens the file. Width and height must be even, as H.264 wants them.
    Result<bool> start(const std::filesystem::path& file, int width, int height, int fps);
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    /// Seconds since the start, for the display.
    [[nodiscard]] double seconds() const;

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
    void write(IMFSinkWriter* writer, unsigned long videoStream, unsigned long audioStream);

    std::atomic<bool> running_{false};
    std::thread thread_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    std::filesystem::path file_;
    std::chrono::steady_clock::time_point started_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::deque<Picture> pictures_;
    std::vector<std::int16_t> sound_;
    std::vector<std::vector<std::uint8_t>> spare_;
    std::size_t dropped_ = 0;
    bool soundArrived_ = false;
    std::string error_;
    RecordingSummary summary_;
};

} // namespace veloplay::record
