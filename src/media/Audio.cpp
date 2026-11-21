#include "media/Audio.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <opus/opus.h>

namespace veloplay::media {

namespace {
// The longest Opus packet holds 120 ms.
constexpr int kMaxPacketMs = 120;
constexpr std::uint32_t kHapticRumbleFloor = 100; // of 65535, as in chiaki-ng
} // namespace

Result<std::unique_ptr<AudioDecoder>> AudioDecoder::create(int sampleRate, int channels) {
    if (channels < 1 || channels > 2) {
        return Error{ErrorCode::InvalidArgument, "only mono and stereo audio are supported"};
    }
    int error = OPUS_OK;
    OpusDecoder* decoder = opus_decoder_create(sampleRate, channels, &error);
    if (!decoder || error != OPUS_OK) {
        return Error{ErrorCode::InvalidArgument, std::string("Opus decoder: ") + opus_strerror(error)};
    }
    std::unique_ptr<AudioDecoder> out(new AudioDecoder());
    out->decoder_ = decoder;
    out->channels_ = channels;
    out->pcm_.resize(static_cast<std::size_t>(sampleRate) * kMaxPacketMs / 1000 * static_cast<std::size_t>(channels));
    return out;
}

AudioDecoder::~AudioDecoder() {
    if (decoder_) opus_decoder_destroy(static_cast<OpusDecoder*>(decoder_));
}

Result<std::span<const std::int16_t>> AudioDecoder::decode(std::span<const std::uint8_t> packet) {
    if (packet.empty()) return Error{ErrorCode::InvalidData, "empty audio packet"};
    const int capacity = static_cast<int>(pcm_.size()) / channels_;
    const int frames = opus_decode(static_cast<OpusDecoder*>(decoder_), packet.data(),
                                   static_cast<opus_int32>(packet.size()), pcm_.data(), capacity, 0);
    if (frames < 0) return Error{ErrorCode::InvalidData, std::string("Opus: ") + opus_strerror(frames)};
    return std::span<const std::int16_t>(pcm_.data(), static_cast<std::size_t>(frames * channels_));
}

AudioBuffer::AudioBuffer(int sampleRate, int channels, std::chrono::milliseconds target,
                         std::chrono::milliseconds limit)
    : sampleRate_(sampleRate),
      channels_(channels),
      targetFrames_(static_cast<std::size_t>(sampleRate) * static_cast<std::size_t>(target.count()) / 1000),
      limitFrames_(std::max(static_cast<std::size_t>(sampleRate) * static_cast<std::size_t>(limit.count()) / 1000,
                            targetFrames_ + 1)) {}

void AudioBuffer::push(std::span<const std::int16_t> interleaved) {
    std::lock_guard lock(mutex_);
    // Make room at the front once the consumed part is the larger one.
    if (readIndex_ > 0 && readIndex_ * 2 >= samples_.size()) {
        samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(readIndex_));
        readIndex_ = 0;
    }
    samples_.insert(samples_.end(), interleaved.begin(), interleaved.end());

    const std::size_t channels = static_cast<std::size_t>(channels_);
    const std::size_t frames = (samples_.size() - readIndex_) / channels;
    if (frames > limitFrames_) {
        const std::size_t drop = frames - targetFrames_;
        readIndex_ += drop * channels;
        dropped_ += drop;
    }
}

std::size_t AudioBuffer::pop(std::span<std::int16_t> out) {
    std::lock_guard lock(mutex_);
    const std::size_t channels = static_cast<std::size_t>(channels_);
    const std::size_t wanted = out.size() / channels;
    const std::size_t available = (samples_.size() - readIndex_) / channels;

    if (filling_) {
        if (available < targetFrames_ || available == 0) {
            std::fill(out.begin(), out.end(), std::int16_t{0});
            return 0;
        }
        filling_ = false;
    }

    const std::size_t real = std::min(wanted, available);
    std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(readIndex_), real * channels, out.begin());
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(real * channels), out.end(), std::int16_t{0});
    readIndex_ += real * channels;
    if (real < wanted) {
        ++underruns_;
        filling_ = true;
    }
    return real;
}

std::chrono::milliseconds AudioBuffer::buffered() const {
    std::lock_guard lock(mutex_);
    const std::size_t frames = (samples_.size() - readIndex_) / static_cast<std::size_t>(channels_);
    return std::chrono::milliseconds(frames * 1000 / static_cast<std::size_t>(sampleRate_));
}

std::uint64_t AudioBuffer::underruns() const {
    std::lock_guard lock(mutex_);
    return underruns_;
}

std::uint64_t AudioBuffer::droppedFrames() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

std::span<const std::int16_t> HapticsUpsampler::convert(std::span<const std::int16_t> stereo) {
    constexpr int factor = kDualSenseAudioRate / kHapticsSampleRate; // 16
    const std::size_t frames = stereo.size() / 2;
    out_.assign(frames * static_cast<std::size_t>(factor) * kDualSenseAudioChannels, 0);
    std::size_t at = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int16_t left = stereo[i * 2];
        const std::int16_t right = stereo[i * 2 + 1];
        // Straight lines between the samples. The actuators answer to a few
        // hundred hertz at most, far below anything this could distort.
        for (int step = 1; step <= factor; ++step) {
            out_[at + 2] = static_cast<std::int16_t>(lastLeft_ + (left - lastLeft_) * step / factor);
            out_[at + 3] = static_cast<std::int16_t>(lastRight_ + (right - lastRight_) * step / factor);
            at += kDualSenseAudioChannels;
        }
        lastLeft_ = left;
        lastRight_ = right;
    }
    return out_;
}

void HapticsUpsampler::reset() {
    lastLeft_ = 0;
    lastRight_ = 0;
}

RumbleLevel hapticsToRumble(std::span<const std::int16_t> stereo) {
    const std::size_t frames = stereo.size() / 2;
    if (frames == 0) return {};
    std::uint64_t sumLeft = 0;
    std::uint64_t sumRight = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        sumLeft += static_cast<std::uint64_t>(std::abs(static_cast<int>(stereo[i * 2])));
        sumRight += static_cast<std::uint64_t>(std::abs(static_cast<int>(stereo[i * 2 + 1])));
    }
    const auto level = [&](std::uint64_t sum) -> std::uint8_t {
        const std::uint64_t strength = std::min<std::uint64_t>(sum * 2 / frames, 65535);
        if (strength <= kHapticRumbleFloor) return 0;
        return static_cast<std::uint8_t>(std::max<std::uint64_t>(strength >> 8, 1));
    };
    return {level(sumLeft), level(sumRight)};
}

} // namespace veloplay::media
