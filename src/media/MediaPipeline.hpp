#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/Audio.hpp"
#include "media/VideoDecoder.hpp"
#include "session/ISession.hpp"

namespace veloplay::media {

/// Access units waiting for the decoder. The network delivers in bursts: a
/// pause, then several frames at once. The decoder works those off in a few
/// milliseconds, so the number waiting says little. What says that the
/// decoder cannot keep up is how long the oldest one has been waiting.
///
/// Not thread-safe; the pipeline guards it.
class DecodeQueue {
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        std::chrono::milliseconds maxWait{250}; // the oldest unit may wait this long
        std::size_t maxUnits = 120;             // a cap on memory, far above any burst
    };

    DecodeQueue() = default;
    explicit DecodeQueue(Limits limits) : limits_(limits) {}

    /// Takes the unit, unless the decoder has fallen behind. Then everything
    /// that waits is thrown away, the unit is not taken either, and the
    /// number of units lost comes back in `dropped`: frames after a gap
    /// cannot be decoded, so the stream has to restart from a key frame.
    bool push(std::vector<std::uint8_t> unit, Clock::time_point now, std::size_t& dropped);

    /// The oldest unit. `since`, if given, receives when it was pushed.
    std::optional<std::vector<std::uint8_t>> pop(Clock::time_point* since = nullptr);
    void clear() { units_.clear(); }
    [[nodiscard]] bool empty() const { return units_.empty(); }
    [[nodiscard]] std::size_t size() const { return units_.size(); }

private:
    struct Entry {
        std::vector<std::uint8_t> unit;
        Clock::time_point since;
    };
    Limits limits_{};
    std::deque<Entry> units_;
};

/// Takes what a session delivers and turns it into pictures and sound:
/// decodes video (the newest picture waits for the renderer), decodes audio
/// into the sound output, and plays haptics on a DualSense if one is there.
///
/// The session calls in from the thread that reads the network. Whatever
/// takes time, above all creating the decoder and decoding, runs on a thread
/// of its own: while the network thread is held up, the console's packets
/// pile up in a small buffer and are lost once it is full.
class MediaPipeline final : public session::IMediaSink {
public:
    struct Options {
        void* d3d11Device = nullptr;      // ID3D11Device* for decoding on the graphics card
        IAudioOutput* audio = nullptr;    // game sound; may be null
        IAudioOutput* haptics = nullptr;  // the DualSense's own sound device; may be null
    };

    struct Stats {
        std::uint64_t decodedFrames = 0;
        std::uint64_t failedFrames = 0;
        std::uint64_t droppedFrames = 0; // thrown away because the decoder fell behind
        double decodeMs = 0.0;           // smoothed time per picture
        double decodeMaxMs = 0.0;        // the slowest picture of the last one to two seconds
        bool hardware = false;
        std::string decoder;             // empty until the first picture
        bool audioPlaying = false;
        bool hapticsPlaying = false;
    };

    explicit MediaPipeline(Options options);
    ~MediaPipeline() override;

    /// Reserve of audio kept against late packets. Takes effect when the
    /// sound output opens, that is at the start of the next session.
    void setAudioDelay(std::chrono::milliseconds delay) { audioDelayMs_.store(static_cast<int>(delay.count())); }

    /// Off means haptics are not played here; the session then offers them as
    /// rumble, which the application ignores when vibration is switched off.
    void setHapticsEnabled(bool enabled) { hapticsEnabled_.store(enabled); }

    // IMediaSink
    void begin(const StreamSettings& stream) override;
    void end() override;
    bool onVideo(std::span<const std::uint8_t> accessUnit, int framesLost, bool recovered) override;
    void onAudioFormat(const session::AudioFormat& format) override;
    void onAudio(std::span<const std::uint8_t> packet) override;
    bool onHaptics(std::span<const std::int16_t> stereo) override;
    bool onPicture(VideoFrame picture) override;
    void onPcmFormat(int sampleRate, int channels) override;
    void onPcm(std::span<const std::int16_t> interleaved) override;

    /// The newest picture that was not handed out yet. Called by the renderer.
    std::optional<VideoFrame> takeFrame();

    /// Every picture decoded since the last call, oldest first. Two pictures
    /// that arrive right behind each other are both handed out; takeFrame()
    /// would only give the newer one, and whoever computes pictures in
    /// between would then bridge twice the motion.
    std::vector<VideoFrame> takeFrames();

    Stats stats();

    /// Waits until everything handed to onVideo() so far is decoded. For tests.
    bool waitUntilIdle(std::chrono::milliseconds timeout);

private:
    void decodeLoop();

    Options options_;
    std::atomic<int> audioDelayMs_{40};
    std::atomic<bool> hapticsEnabled_{true};

    // Network thread only, between begin() and end().
    std::unique_ptr<AudioDecoder> audio_;
    bool pcm_ = false; // sound arrives as samples and goes straight to the output
    HapticsUpsampler upsampler_;

    // Decode thread only.
    std::unique_ptr<VideoDecoder> video_;
    VideoCodec codec_ = VideoCodec::Hevc;

    std::thread decodeThread_;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    DecodeQueue queue_;
    bool decoding_ = false;
    bool stopping_ = false;
    /// Set when a picture was lost on our side; the next onVideo() reports it.
    std::atomic<bool> pictureLost_{false};

    std::mutex mutex_;
    std::deque<VideoFrame> pending_; // decoded and not fetched yet; a handful at most
    Stats stats_;
};

} // namespace veloplay::media
