#pragma once

#include <array>
#include <functional>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "util/Result.hpp"

namespace veloplay::media {

/// Plays 16-bit PCM. The Windows client implements it with WASAPI.
class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    /// Starts opening the device and returns at once, because it is called
    /// from the thread that reads the network. `delay` is how much audio is
    /// kept in reserve against packets that arrive late. False means it
    /// cannot work at all; whether the device really opened shows in running().
    virtual bool start(int sampleRate, int channels, std::chrono::milliseconds delay) = 0;
    /// Queues interleaved samples. Never blocks.
    virtual void write(std::span<const std::int16_t> interleaved) = 0;
    virtual void stop() = 0;
    /// True while the device is open and playing.
    [[nodiscard]] virtual bool running() const = 0;
};

// The microphone's sound travels to the console in a fixed shape: 48 kHz,
// stereo, blocks of 480 samples per channel (10 ms), each packed by Opus
// into exactly 40 bytes. The console drops packets of any other size.
inline constexpr int kMicSampleRate = 48000;
inline constexpr int kMicChannels = 2;
inline constexpr int kMicFrameSamples = 480;
inline constexpr std::size_t kMicPacketSize = 40;

/// Records 16-bit PCM in the microphone's shape. The Windows client implements it with WASAPI.
class IAudioInput {
public:
    /// One block: kMicFrameSamples pairs of left and right. Called on a thread of the device.
    using Sink = std::function<void(std::span<const std::int16_t> interleaved)>;

    virtual ~IAudioInput() = default;

    /// Starts looking for the default microphone and returns at once. While
    /// started it keeps looking: a microphone that is plugged in later is
    /// picked up, and one that Windows makes the default is followed.
    virtual bool start(Sink sink) = 0;
    /// Returns once no call to the sink is running and none will follow.
    virtual void stop() = 0;
    /// True from start() until stop(), unless it gave up for good.
    [[nodiscard]] virtual bool running() const = 0;
    /// True while a microphone is open and its sound reaches the sink.
    [[nodiscard]] virtual bool recording() const = 0;
};

/// Packs blocks of the microphone's sound for the console.
class MicEncoder {
public:
    MicEncoder();
    ~MicEncoder();
    MicEncoder(const MicEncoder&) = delete;
    MicEncoder& operator=(const MicEncoder&) = delete;

    [[nodiscard]] bool valid() const { return encoder_ != nullptr; }
    /// One block of kMicFrameSamples pairs in, one packet of kMicPacketSize
    /// bytes out; empty if the block has another size or packing failed.
    /// The packet stays valid until the next call.
    std::span<const std::uint8_t> encode(std::span<const std::int16_t> interleaved);

private:
    void* encoder_ = nullptr; // OpusEncoder
    std::array<std::uint8_t, kMicPacketSize> packet_{};
};

/// Decodes the console's Opus packets.
class AudioDecoder {
public:
    static Result<std::unique_ptr<AudioDecoder>> create(int sampleRate, int channels);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /// Interleaved samples of one packet. The span is valid until the next call.
    Result<std::span<const std::int16_t>> decode(std::span<const std::uint8_t> packet);

    [[nodiscard]] int channels() const { return channels_; }

private:
    AudioDecoder() = default;
    void* decoder_ = nullptr; // OpusDecoder*
    int channels_ = 0;
    std::vector<std::int16_t> pcm_;
};

/// Sits between the network, which delivers audio in bursts, and the sound
/// card, which wants it evenly. One thread pushes, another pops.
///
/// Playback starts once `target` worth of audio is queued. If the queue runs
/// dry it plays silence and fills up to `target` again. If it grows beyond
/// `limit`, the oldest audio is dropped, so that sound never trails the picture
/// by more than that.
class AudioBuffer {
public:
    AudioBuffer(int sampleRate, int channels, std::chrono::milliseconds target, std::chrono::milliseconds limit);

    void push(std::span<const std::int16_t> interleaved);

    /// Always fills `out`. Returns how many of the frames were real audio.
    std::size_t pop(std::span<std::int16_t> out);

    [[nodiscard]] std::chrono::milliseconds buffered() const;
    [[nodiscard]] std::uint64_t underruns() const;
    [[nodiscard]] std::uint64_t droppedFrames() const;
    [[nodiscard]] int channels() const { return channels_; }
    [[nodiscard]] int sampleRate() const { return sampleRate_; }

private:
    const int sampleRate_;
    const int channels_;
    const std::size_t targetFrames_;
    const std::size_t limitFrames_;

    mutable std::mutex mutex_;
    std::vector<std::int16_t> samples_;
    std::size_t readIndex_ = 0; // in samples
    bool filling_ = true;
    std::uint64_t underruns_ = 0;
    std::uint64_t dropped_ = 0;
};

// ---- DualSense haptics -----------------------------------------------------
// With DualSense features switched on, the console sends vibration as a second
// audio stream: stereo, 16 bit, 3000 Hz, ten milliseconds per packet.

inline constexpr int kHapticsSampleRate = 3000;
inline constexpr int kDualSenseAudioRate = 48000;
inline constexpr int kDualSenseAudioChannels = 4;

/// Converts haptics audio into what the DualSense's own sound device takes
/// over USB: 48 kHz, four channels, the actuators on channels three and four.
/// Channels one and two are the controller's speaker and stay silent.
class HapticsUpsampler {
public:
    /// `stereo` holds left/right pairs. Returns interleaved four-channel audio.
    std::span<const std::int16_t> convert(std::span<const std::int16_t> stereo);
    void reset();

private:
    std::int16_t lastLeft_ = 0;
    std::int16_t lastRight_ = 0;
    std::vector<std::int16_t> out_;
};

struct RumbleLevel {
    std::uint8_t low = 0;  // from the left actuator
    std::uint8_t high = 0; // from the right actuator

    friend bool operator==(const RumbleLevel&, const RumbleLevel&) = default;
};

/// Loudness of a haptics packet as motor strengths, for controllers that
/// cannot play the audio itself. Follows chiaki-ng: twice the mean amplitude,
/// with a floor that keeps idle noise from buzzing.
RumbleLevel hapticsToRumble(std::span<const std::int16_t> stereo);

} // namespace veloplay::media
