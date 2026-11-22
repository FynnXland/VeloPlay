#include "media/MediaPipeline.hpp"

#include <spdlog/spdlog.h>

namespace veloplay::media {

MediaPipeline::MediaPipeline(Options options) : options_(options) {}

MediaPipeline::~MediaPipeline() { end(); }

void MediaPipeline::begin(const StreamSettings& stream) {
    end();
    codec_ = stream.codec;
    upsampler_.reset();
    pictureLost_.store(false);
    {
        std::lock_guard lock(mutex_);
        stats_ = Stats{};
    }
    {
        std::lock_guard lock(queueMutex_);
        stopping_ = false;
    }
    decodeThread_ = std::thread([this] { decodeLoop(); });
    // Just enough reserve to bridge the ten milliseconds between packets.
    if (options_.haptics) {
        options_.haptics->start(kDualSenseAudioRate, kDualSenseAudioChannels, std::chrono::milliseconds{20});
    }
}

void MediaPipeline::end() {
    if (decodeThread_.joinable()) {
        {
            std::lock_guard lock(queueMutex_);
            stopping_ = true;
            queue_.clear();
        }
        queueChanged_.notify_all();
        decodeThread_.join();
    }
    {
        // Pictures hold on to the decoder's buffers, so they go first.
        std::lock_guard lock(mutex_);
        pending_.clear();
    }
    video_.reset();
    audio_.reset();
    pcm_ = false;
    if (options_.audio) options_.audio->stop();
    if (options_.haptics) options_.haptics->stop();
}

bool DecodeQueue::push(std::vector<std::uint8_t> unit, Clock::time_point now, std::size_t& dropped) {
    dropped = 0;
    const bool late = !units_.empty() && now - units_.front().since > limits_.maxWait;
    if (late || units_.size() >= limits_.maxUnits) {
        dropped = units_.size() + 1;
        units_.clear();
        return false;
    }
    units_.push_back({std::move(unit), now});
    return true;
}

std::optional<std::vector<std::uint8_t>> DecodeQueue::pop(Clock::time_point* since) {
    if (units_.empty()) return std::nullopt;
    if (since) *since = units_.front().since;
    std::vector<std::uint8_t> unit = std::move(units_.front().unit);
    units_.pop_front();
    return unit;
}

bool MediaPipeline::onVideo(std::span<const std::uint8_t> accessUnit, int, bool) {
    // A picture was lost on our side since the last call. What follows refers
    // to it, so this unit is of no use either. Saying so makes the session
    // ask the console for a key frame and hold back everything until then.
    if (pictureLost_.exchange(false)) return false;

    std::size_t dropped = 0;
    bool taken = false;
    {
        std::lock_guard lock(queueMutex_);
        if (stopping_ || !decodeThread_.joinable()) return false;
        // Asked once more under the lock: setDevice() sets it while it empties the queue.
        if (pictureLost_.exchange(false)) return false;
        taken = queue_.push(std::vector<std::uint8_t>(accessUnit.begin(), accessUnit.end()),
                            std::chrono::steady_clock::now(), dropped);
    }
    if (!taken) {
        std::lock_guard lock(mutex_);
        stats_.droppedFrames += dropped;
        spdlog::warn("The video decoder fell behind; {} frames were dropped ({} so far)", dropped,
                     stats_.droppedFrames);
        return false;
    }
    queueChanged_.notify_all();
    return true;
}

void MediaPipeline::setDevice(void* d3d11Device) {
    {
        std::lock_guard lock(deviceMutex_);
        options_.d3d11Device = d3d11Device;
    }
    {
        std::lock_guard lock(mutex_);
        ++deviceGeneration_;
        pending_.clear();
    }
    // What waits refers to pictures the old decoder had; the new one starts from a whole picture.
    std::lock_guard lock(queueMutex_);
    queue_.clear();
    pictureLost_.store(true);
}

void MediaPipeline::decodeLoop() {
    bool failedForGood = false;
    std::uint64_t generation = 0; // of the device the decoder was made on
    {
        std::lock_guard lock(mutex_);
        generation = deviceGeneration_;
    }
    // The average hides what makes a stream stutter: one picture in thirty
    // that takes far longer than the rest. So the slowest one is kept too,
    // over this second and the one before.
    double slowestNow = 0.0;
    double slowestBefore = 0.0;
    std::chrono::steady_clock::time_point slowestSince{};
    while (true) {
        std::vector<std::uint8_t> unit;
        std::chrono::steady_clock::time_point received{};
        {
            std::unique_lock lock(queueMutex_);
            decoding_ = false;
            queueChanged_.notify_all();
            queueChanged_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            unit = std::move(*queue_.pop(&received));
            decoding_ = true;
        }
        {
            std::lock_guard lock(mutex_);
            if (generation != deviceGeneration_) {
                generation = deviceGeneration_;
                video_.reset();
                failedForGood = false;
            }
        }
        if (failedForGood) continue;

        if (!video_) {
            std::lock_guard lock(deviceMutex_);
            auto created = VideoDecoder::create({codec_, options_.d3d11Device});
            if (!created) {
                spdlog::error("Video decoder: {}", created.error().message);
                failedForGood = true;
                continue;
            }
            video_ = std::move(*created);
        }

        const auto started = std::chrono::steady_clock::now();
        auto decoded = video_->decode(unit);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (started - slowestSince >= std::chrono::seconds(1)) {
            slowestBefore = slowestNow;
            slowestNow = 0.0;
            slowestSince = started;
        }
        slowestNow = std::max(slowestNow, ms);

        std::lock_guard lock(mutex_);
        // Made on a device that was replaced meanwhile: its pictures cannot go to the new one.
        if (generation != deviceGeneration_) continue;
        stats_.decodeMaxMs = std::max(slowestNow, slowestBefore);
        if (!decoded) {
            if (stats_.failedFrames++ % 60 == 0) {
                spdlog::warn("Video frame could not be decoded: {}", decoded.error().message);
            }
            pictureLost_.store(true);
            continue;
        }
        if (decoded->has_value()) {
            if (stats_.decodedFrames == 0) {
                spdlog::info("First picture: {}x{}, {}", (*decoded)->width, (*decoded)->height, video_->description());
            }
            ++stats_.decodedFrames;
            stats_.decodeMs = stats_.decodeMs == 0.0 ? ms : stats_.decodeMs * 0.9 + ms * 0.1;
            stats_.hardware = video_->hardware();
            stats_.decoder = video_->description();
            (*decoded)->receivedAt = received;
            (*decoded)->decodedAt = std::chrono::steady_clock::now();
            pending_.push_back(std::move(**decoded));
            // Nobody fetches: keep the newest few, so a window that is busy does not pile up pictures.
            while (pending_.size() > 4) pending_.pop_front();
        }
    }
}

bool MediaPipeline::waitUntilIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(queueMutex_);
    return queueChanged_.wait_for(lock, timeout, [&] { return queue_.empty() && !decoding_; });
}

void MediaPipeline::onAudioFormat(const session::AudioFormat& format) {
    audio_.reset();
    if (!options_.audio) return;
    auto created = AudioDecoder::create(static_cast<int>(format.sampleRate), static_cast<int>(format.channels));
    if (!created) {
        spdlog::error("Audio decoder: {}", created.error().message);
        return;
    }
    // Returns at once; the device is opened in the background.
    if (options_.audio->start(static_cast<int>(format.sampleRate), static_cast<int>(format.channels),
                              std::chrono::milliseconds{audioDelayMs_.load()})) {
        audio_ = std::move(*created);
        spdlog::info("Audio: {} Hz, {} channels", format.sampleRate, format.channels);
    }
}

void MediaPipeline::onAudio(std::span<const std::uint8_t> packet) {
    if (!audio_) return;
    const auto pcm = audio_->decode(packet);
    if (pcm) options_.audio->write(*pcm);
}

bool MediaPipeline::onHaptics(std::span<const std::int16_t> stereo) {
    if (!options_.haptics || !hapticsEnabled_.load() || !options_.haptics->running()) return false;
    options_.haptics->write(upsampler_.convert(stereo));
    return true;
}

bool MediaPipeline::onPicture(VideoFrame picture) {
    if (picture.width <= 0 || picture.height <= 0 || picture.layout == PixelLayout::D3D11Nv12 || !picture.planes[0]) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    if (stats_.decodedFrames == 0) spdlog::info("First picture: {}x{}, as it is", picture.width, picture.height);
    ++stats_.decodedFrames;
    picture.sequence = stats_.decodedFrames;
    if (picture.receivedAt == std::chrono::steady_clock::time_point{}) picture.receivedAt = now;
    picture.decodedAt = now;
    stats_.decoder = "none";
    pending_.push_back(std::move(picture));
    while (pending_.size() > 4) pending_.pop_front();
    return true;
}

void MediaPipeline::onPcmFormat(int sampleRate, int channels) {
    audio_.reset();
    pcm_ = false;
    if (!options_.audio || sampleRate <= 0 || channels <= 0) return;
    if (options_.audio->start(sampleRate, channels, std::chrono::milliseconds{audioDelayMs_.load()})) {
        pcm_ = true;
        spdlog::info("Audio: {} Hz, {} channels, as it is", sampleRate, channels);
    }
}

void MediaPipeline::onPcm(std::span<const std::int16_t> interleaved) {
    if (pcm_) options_.audio->write(interleaved);
}

std::optional<VideoFrame> MediaPipeline::takeFrame() {
    std::lock_guard lock(mutex_);
    std::optional<VideoFrame> out;
    if (!pending_.empty()) out = std::move(pending_.back());
    pending_.clear();
    return out;
}

std::vector<VideoFrame> MediaPipeline::takeFrames() {
    std::lock_guard lock(mutex_);
    std::vector<VideoFrame> out(std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
    pending_.clear();
    return out;
}

MediaPipeline::Stats MediaPipeline::stats() {
    Stats out;
    {
        std::lock_guard lock(mutex_);
        out = stats_;
    }
    out.audioPlaying = options_.audio && options_.audio->running();
    out.hapticsPlaying = options_.haptics && options_.haptics->running();
    return out;
}

} // namespace veloplay::media
