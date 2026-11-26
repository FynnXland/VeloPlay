#include "linux/SdlAudio.hpp"

#include <algorithm>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

namespace veloplay::linuxos {

namespace {

// Audio older than this beyond the reserve is dropped instead of played late (as WasapiOutput).
constexpr std::chrono::milliseconds kExtraBeforeDrop{80};
// How often the threads look at the device again: a lost device, a microphone to find.
constexpr std::chrono::milliseconds kLookAgain{1000};
constexpr std::chrono::milliseconds kMicLookAgain{2000};
// A microphone stream that has delivered nothing for this long sits on a device that is gone.
constexpr std::chrono::milliseconds kMicSilentFor{2000};

/// The device buffers little: the reserve against late packets lives in
/// AudioBuffer. 10 ms at 48 kHz; only a default, the user's SDL settings win.
void preferSmallDeviceBuffer() {
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480", SDL_HINT_DEFAULT);
}

bool audioReady(bool& logged) {
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0) return true;
    if (!logged) {
        spdlog::error("Sound: SDL's audio is not initialised; the program must call SDL_InitSubSystem(SDL_INIT_AUDIO)");
        logged = true;
    }
    return false;
}

std::vector<SDL_AudioDeviceID> playbackDevices() {
    int count = 0;
    SDL_AudioDeviceID* list = SDL_GetAudioPlaybackDevices(&count);
    std::vector<SDL_AudioDeviceID> devices;
    if (list) devices.assign(list, list + std::max(count, 0));
    SDL_free(list);
    return devices;
}

/// The first output whose name holds a needle and that has `channels`; 0 if none.
SDL_AudioDeviceID findDevice(const std::vector<std::string>& needles, int channels, bool& warnedChannels) {
    for (const SDL_AudioDeviceID device : playbackDevices()) {
        const char* name = SDL_GetAudioDeviceName(device);
        if (!name) continue;
        const std::string_view text(name);
        if (std::none_of(needles.begin(), needles.end(),
                         [&](const std::string& needle) { return text.find(needle) != std::string_view::npos; })) {
            continue;
        }
        SDL_AudioSpec spec{};
        if (SDL_GetAudioDeviceFormat(device, &spec, nullptr) && spec.channels < channels) {
            // A device picked by name must really have the channels, or the haptics would be mixed into the speaker.
            if (!warnedChannels) {
                spdlog::warn("The controller's sound device \"{}\" offers {} channels, {} are needed. Choose the "
                             "four-channel profile (\"Surround 4.0\") for it in the sound settings to get haptics.",
                             name, spec.channels, channels);
                warnedChannels = true;
            }
            continue;
        }
        return device;
    }
    return 0;
}

bool listed(SDL_AudioDeviceID device) {
    const std::vector<SDL_AudioDeviceID> devices = playbackDevices();
    return std::find(devices.begin(), devices.end(), device) != devices.end();
}

std::int64_t nowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ---- output --------------------------------------------------------------------------

SdlAudioOutput::SdlAudioOutput() = default;

SdlAudioOutput::SdlAudioOutput(std::vector<std::string> deviceNameNeedles) : needles_(std::move(deviceNameNeedles)) {}

SdlAudioOutput::~SdlAudioOutput() {
    stop();
}

std::vector<std::string> SdlAudioOutput::deviceNames() {
    std::vector<std::string> names;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) return names;
    for (const SDL_AudioDeviceID device : playbackDevices()) {
        if (const char* name = SDL_GetAudioDeviceName(device)) names.emplace_back(name);
    }
    return names;
}

void SdlAudioOutput::setDelay(std::chrono::milliseconds delay) {
    std::lock_guard lock(mutex_);
    if (buffer_) buffer_->setTarget(delay, delay + kExtraBeforeDrop);
}

std::uint64_t SdlAudioOutput::underruns() const {
    std::lock_guard lock(mutex_);
    return buffer_ ? buffer_->underruns() : 0;
}

void SdlAudioOutput::setTap(Tap tap) {
    std::lock_guard lock(mutex_);
    tap_ = std::move(tap);
}

void SdlAudioOutput::write(std::span<const std::int16_t> interleaved) {
    if (!running_.load()) return;
    std::lock_guard lock(mutex_);
    if (buffer_) buffer_->push(interleaved);
}

bool SdlAudioOutput::start(int sampleRate, int channels, std::chrono::milliseconds delay) {
    stop();
    if (sampleRate <= 0 || channels <= 0) return false;
    {
        std::lock_guard lock(mutex_);
        buffer_ = std::make_unique<media::AudioBuffer>(sampleRate, channels, delay, delay + kExtraBeforeDrop);
    }
    {
        std::lock_guard lock(stopMutex_);
        stopping_ = false;
    }
    // Opened on a thread of its own: start() is called from the thread that reads the network.
    std::promise<bool> opened;
    opened_ = opened.get_future();
    thread_ = std::thread([this, sampleRate, channels, promise = std::move(opened)]() mutable {
        run(sampleRate, channels, std::move(promise));
    });
    return true;
}

bool SdlAudioOutput::waitForStop(std::chrono::milliseconds time) {
    std::unique_lock lock(stopMutex_);
    return stopWake_.wait_for(lock, time, [this] { return stopping_; });
}

void SdlAudioOutput::run(int sampleRate, int channels, std::promise<bool> opened) {
    const bool follow = needles_.empty();
    bool reported = false;
    bool silentLogged = false;
    bool initLogged = false;
    bool warnedChannels = false;
    const auto report = [&](bool ok) {
        if (!reported) opened.set_value(ok);
        reported = true;
    };
    preferSmallDeviceBuffer();
    while (true) {
        SDL_AudioStream* stream = nullptr;
        SDL_AudioDeviceID device = 0;
        if (audioReady(initLogged)) {
            device = follow ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : findDevice(needles_, channels, warnedChannels);
            if (device != 0) {
                const SDL_AudioSpec spec{SDL_AUDIO_S16, channels, sampleRate};
                stream = SDL_OpenAudioDeviceStream(device, &spec, &SdlAudioOutput::feed, this);
                if (stream && !SDL_ResumeAudioStreamDevice(stream)) {
                    SDL_DestroyAudioStream(stream);
                    stream = nullptr;
                }
                if (!stream) spdlog::warn("Sound: the device could not be opened: {}", SDL_GetError());
            }
        }
        const bool ok = stream != nullptr;
        running_.store(ok);
        report(ok);
        if (ok) {
            spdlog::info("Sound output open: {} Hz, {} channels{}", sampleRate, channels,
                         follow ? "" : " (controller)");
            silentLogged = false;
        } else if (follow && !silentLogged) {
            spdlog::warn("No sound device could be opened; the stream stays silent until there is one");
            silentLogged = true;
        }
        if (!ok) {
            if (!follow) break;
            if (waitForStop(kLookAgain)) break;
            continue; // nothing to play on right now; look again in a moment
        }

        // SDL plays from here on. This thread only watches: is the device picked by name still there?
        bool stopping = false;
        while (!(stopping = waitForStop(kLookAgain))) {
            if (!follow && !listed(device)) {
                spdlog::warn("The controller's sound device is gone");
                break;
            }
        }
        // Not under mutex_: SDL's thread may be inside feed() waiting for it, and this waits for that thread.
        SDL_DestroyAudioStream(stream);
        running_.store(false);
        if (stopping || !follow) break;
    }
    report(false);
    running_.store(false);
}

void SdlAudioOutput::feed(void* self, SDL_AudioStream* stream, int additional, int) {
    auto& output = *static_cast<SdlAudioOutput*>(self);
    if (additional <= 0) return;
    {
        std::lock_guard lock(output.mutex_);
        if (!output.buffer_) return;
        const int channels = output.buffer_->channels();
        const std::size_t frameBytes = static_cast<std::size_t>(channels) * sizeof(std::int16_t);
        const std::size_t frames = (static_cast<std::size_t>(additional) + frameBytes - 1) / frameBytes;
        output.block_.resize(frames * static_cast<std::size_t>(channels));
        output.buffer_->pop(output.block_); // always fills: silence where the network has nothing yet
        if (output.tap_) output.tap_(output.block_, output.buffer_->sampleRate(), channels);
    }
    SDL_PutAudioStreamData(stream, output.block_.data(), static_cast<int>(output.block_.size() * sizeof(std::int16_t)));
}

bool SdlAudioOutput::waitUntilOpened(std::chrono::milliseconds timeout) {
    if (!opened_.valid()) return false;
    if (opened_.wait_for(timeout) != std::future_status::ready) return false;
    return running_.load();
}

void SdlAudioOutput::stop() {
    if (thread_.joinable()) {
        {
            std::lock_guard lock(stopMutex_);
            stopping_ = true;
        }
        stopWake_.notify_all();
        thread_.join();
    }
    running_.store(false);
    std::lock_guard lock(mutex_);
    buffer_.reset();
}

// ---- microphone ----------------------------------------------------------------------

SdlAudioInput::~SdlAudioInput() {
    stop();
}

bool SdlAudioInput::start(Sink sink) {
    stop();
    if (!sink) return false;
    {
        std::lock_guard lock(sinkMutex_);
        sink_ = std::move(sink);
        delivering_ = true;
        pending_.clear();
    }
    {
        std::lock_guard lock(stopMutex_);
        stopping_ = false;
    }
    running_.store(true);
    // Opened on a thread of its own: opening can take a while, and the caller draws the window.
    thread_ = std::thread([this] { run(); });
    return true;
}

bool SdlAudioInput::waitForStop(std::chrono::milliseconds time) {
    std::unique_lock lock(stopMutex_);
    return stopWake_.wait_for(lock, time, [this] { return stopping_; });
}

void SdlAudioInput::run() {
    bool initLogged = false;
    bool missingLogged = false;
    preferSmallDeviceBuffer();
    while (true) {
        SDL_AudioStream* stream = nullptr;
        if (audioReady(initLogged)) {
            const SDL_AudioSpec spec{SDL_AUDIO_S16, media::kMicChannels, media::kMicSampleRate};
            // SDL turns a mono or 44.1 kHz microphone into what is asked for here, and follows the default.
            stream =
                SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, &SdlAudioInput::collect, this);
            if (stream && !SDL_ResumeAudioStreamDevice(stream)) {
                SDL_DestroyAudioStream(stream);
                stream = nullptr;
            }
        }
        if (!stream) {
            if (!missingLogged) {
                spdlog::warn("No microphone could be opened ({}); looking again every {} ms", SDL_GetError(),
                             kMicLookAgain.count());
                missingLogged = true;
            }
            if (waitForStop(kMicLookAgain)) break;
            continue;
        }
        missingLogged = false;
        lastSound_.store(nowNanoseconds());
        recording_.store(true);
        spdlog::info("Microphone open: {} Hz, {} channels", media::kMicSampleRate, media::kMicChannels);

        bool stopping = false;
        while (!(stopping = waitForStop(std::chrono::milliseconds(500)))) {
            const auto silent = std::chrono::nanoseconds(nowNanoseconds() - lastSound_.load());
            if (silent > kMicSilentFor) {
                // A stream left on a device that went away delivers nothing; open whatever is there now.
                spdlog::warn("The microphone delivers nothing; looking for another one");
                break;
            }
        }
        SDL_DestroyAudioStream(stream); // not under sinkMutex_: SDL's thread may wait for it in collect()
        recording_.store(false);
        if (stopping) break;
        {
            std::lock_guard lock(sinkMutex_);
            pending_.clear();
        }
        if (waitForStop(std::chrono::milliseconds(200))) break;
    }
    recording_.store(false);
    running_.store(false);
}

void SdlAudioInput::collect(void* self, SDL_AudioStream* stream, int, int) {
    auto& input = *static_cast<SdlAudioInput*>(self);
    const int available = SDL_GetAudioStreamAvailable(stream);
    if (available <= 0) return;
    input.read_.resize(static_cast<std::size_t>(available) / sizeof(std::int16_t));
    const int got = SDL_GetAudioStreamData(stream, input.read_.data(), available);
    if (got <= 0) return;
    input.lastSound_.store(nowNanoseconds());

    std::lock_guard lock(input.sinkMutex_);
    if (!input.delivering_) return;
    input.pending_.insert(input.pending_.end(), input.read_.begin(),
                          input.read_.begin() +
                              static_cast<std::ptrdiff_t>(static_cast<std::size_t>(got) / sizeof(std::int16_t)));
    const std::size_t blockSamples = static_cast<std::size_t>(media::kMicFrameSamples * media::kMicChannels);
    std::size_t done = 0;
    while (input.pending_.size() - done >= blockSamples) {
        input.sink_(std::span<const std::int16_t>(input.pending_.data() + done, blockSamples));
        done += blockSamples;
    }
    if (done > 0)
        input.pending_.erase(input.pending_.begin(), input.pending_.begin() + static_cast<std::ptrdiff_t>(done));
}

void SdlAudioInput::stop() {
    {
        // Once this is through, no call to the sink runs and none will follow.
        std::lock_guard lock(sinkMutex_);
        delivering_ = false;
    }
    if (thread_.joinable()) {
        {
            std::lock_guard lock(stopMutex_);
            stopping_ = true;
        }
        stopWake_.notify_all();
        thread_.join();
    }
    {
        std::lock_guard lock(sinkMutex_);
        sink_ = nullptr;
        pending_.clear();
    }
    running_.store(false);
    recording_.store(false);
}

} // namespace veloplay::linuxos
