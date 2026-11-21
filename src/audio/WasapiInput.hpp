#pragma once

#include <atomic>
#include <thread>

#include "media/Audio.hpp"

namespace veloplay::audio {

/// The default microphone through WASAPI in shared mode. Windows converts
/// whatever the device records to the shape the console wants.
class WasapiInput final : public media::IAudioInput {
public:
    WasapiInput();
    ~WasapiInput() override;

    bool start(Sink sink) override;
    void stop() override;
    [[nodiscard]] bool running() const override { return running_.load(); }
    [[nodiscard]] bool recording() const override { return recording_.load(); }

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> recording_{false};
    void* stopEvent_ = nullptr; // HANDLE
};

} // namespace veloplay::audio
