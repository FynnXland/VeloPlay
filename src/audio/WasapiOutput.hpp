#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/Audio.hpp"

namespace veloplay::audio {

/// Sound output through WASAPI in shared mode. Windows converts our format to
/// whatever the device runs at.
class WasapiOutput final : public media::IAudioOutput {
public:
    /// Without needles the default output device is used. With needles, the
    /// first active output whose name contains one of them, and only if it
    /// offers at least as many channels as are asked for in start(). That is
    /// how the DualSense's four-channel device is found for haptics.
    explicit WasapiOutput(std::vector<std::wstring> deviceNameNeedles = {});
    ~WasapiOutput() override;

    bool start(int sampleRate, int channels, std::chrono::milliseconds delay) override;
    void write(std::span<const std::int16_t> interleaved) override;
    void stop() override;
    [[nodiscard]] bool running() const override { return running_.load(); }

    /// Waits for the outcome of start(): true once the device plays, false if
    /// it could not be opened or the time ran out.
    bool waitUntilOpened(std::chrono::milliseconds timeout);

    /// Gets every block of sound as it goes to the device, on the device's
    /// thread: what a recording hears. Must return quickly.
    using Tap = std::function<void(std::span<const std::int16_t> interleaved, int sampleRate, int channels)>;
    void setTap(Tap tap);

private:
    const std::vector<std::wstring> needles_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::future<bool> opened_;
    void* stopEvent_ = nullptr; // HANDLE

    std::mutex mutex_; // guards buffer_ against start() and stop()
    std::unique_ptr<media::AudioBuffer> buffer_;
    Tap tap_; // guarded by mutex_
};

} // namespace veloplay::audio
