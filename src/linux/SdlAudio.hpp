#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "media/Audio.hpp"

struct SDL_AudioStream;

namespace veloplay::linuxos {

// Sound through SDL3, which talks to PipeWire, PulseAudio or ALSA, whatever
// the system has. The shell calls SDL_InitSubSystem(SDL_INIT_AUDIO) once on
// the main thread before these are started: SDL counts its subsystems without
// locks, so the threads here never do it themselves. Until it has, a start
// opens nothing and the default output keeps looking.

/// The Linux counterpart of WasapiOutput: the same buffer between the network
/// and the device (media::AudioBuffer), the same reserve, and the same tap.
/// SDL asks for sound from a thread of its own whenever the device needs more,
/// and gets exactly that much from the buffer.
class SdlAudioOutput final : public media::IAudioOutput {
public:
    /// The default output. When the desktop makes another device the default
    /// (headphones plugged in), SDL moves the sound there by itself.
    SdlAudioOutput();
    /// The first output whose name contains one of the needles, and only if it
    /// has at least the channels asked for in start(): how the DualSense's own
    /// four-channel device is found for haptics ("DualSense", "Wireless
    /// Controller"). Opened once; when it goes away the output stops.
    explicit SdlAudioOutput(std::vector<std::string> deviceNameNeedles);
    ~SdlAudioOutput() override;
    SdlAudioOutput(const SdlAudioOutput&) = delete;
    SdlAudioOutput& operator=(const SdlAudioOutput&) = delete;

    /// Returns at once; the device is opened on a thread of this output's own.
    bool start(int sampleRate, int channels, std::chrono::milliseconds delay) override;
    void write(std::span<const std::int16_t> interleaved) override;
    void stop() override;
    [[nodiscard]] bool running() const override { return running_.load(); }
    void setDelay(std::chrono::milliseconds delay) override;
    [[nodiscard]] std::uint64_t underruns() const override;

    /// Waits for the outcome of start(): true once the device plays.
    bool waitUntilOpened(std::chrono::milliseconds timeout);

    /// Gets every block of sound as it goes to the device, on SDL's thread:
    /// what a recording hears. Must return quickly.
    using Tap = std::function<void(std::span<const std::int16_t> interleaved, int sampleRate, int channels)>;
    void setTap(Tap tap);

    /// The names of the outputs SDL knows right now (for the log and tests).
    static std::vector<std::string> deviceNames();

private:
    static void feed(void* self, SDL_AudioStream* stream, int additional, int total);
    void run(int sampleRate, int channels, std::promise<bool> opened);
    bool waitForStop(std::chrono::milliseconds time);

    const std::vector<std::string> needles_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::future<bool> opened_;

    std::mutex stopMutex_;
    std::condition_variable stopWake_;
    bool stopping_ = false;

    mutable std::mutex mutex_; // guards buffer_ and tap_ against start(), stop() and SDL's thread
    std::unique_ptr<media::AudioBuffer> buffer_;
    Tap tap_;
    std::vector<std::int16_t> block_; // SDL's thread only
};

/// The Linux counterpart of WasapiInput: the default microphone, 48 kHz
/// stereo 16 bit, handed on in blocks of media::kMicFrameSamples. While
/// started it keeps looking: a microphone plugged in later is picked up, and
/// one that falls silent for two seconds is opened anew.
class SdlAudioInput final : public media::IAudioInput {
public:
    SdlAudioInput() = default;
    ~SdlAudioInput() override;
    SdlAudioInput(const SdlAudioInput&) = delete;
    SdlAudioInput& operator=(const SdlAudioInput&) = delete;

    bool start(Sink sink) override;
    /// Returns once no call to the sink is running and none will follow.
    void stop() override;
    [[nodiscard]] bool running() const override { return running_.load(); }
    [[nodiscard]] bool recording() const override { return recording_.load(); }

private:
    static void collect(void* self, SDL_AudioStream* stream, int additional, int total);
    void run();
    bool waitForStop(std::chrono::milliseconds time);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> recording_{false};
    std::atomic<std::int64_t> lastSound_{0}; // steady clock, nanoseconds

    std::mutex stopMutex_;
    std::condition_variable stopWake_;
    bool stopping_ = false;

    std::mutex sinkMutex_; // held while the sink runs; stop() takes it to know none does
    Sink sink_;
    bool delivering_ = false;
    std::vector<std::int16_t> pending_; // SDL's thread only, under sinkMutex_
    std::vector<std::int16_t> read_;
};

} // namespace veloplay::linuxos
