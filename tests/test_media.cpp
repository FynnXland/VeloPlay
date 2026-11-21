#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>

#include <opus/opus.h>

#include "media/Audio.hpp"
#include "media/FramePacer.hpp"
#include "media/MediaPipeline.hpp"
#include "media/VideoDecoder.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

// tests/data holds two clips made with ffmpeg: six frames of one colour,
// 128x72, no reordered frames, an access unit delimiter in front of each
// frame, BT.709 with limited range.
//   clip.h264: Y 88, U 118, V 196        clip.h265: Y 91, U 180, V 93
Bytes readClip(const char* name) {
    std::ifstream file(std::string(VELOPLAY_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/// Cuts an Annex B stream in front of every access unit delimiter.
std::vector<Bytes> accessUnits(const Bytes& stream, bool hevc) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 4 < stream.size(); ++i) {
        if (stream[i] != 0 || stream[i + 1] != 0 || stream[i + 2] != 1) continue;
        const std::uint8_t header = stream[i + 3];
        const bool delimiter = hevc ? ((header >> 1) & 0x3f) == 35 : (header & 0x1f) == 9;
        // Keep the leading zero of a four-byte start code with its unit.
        if (delimiter) starts.push_back(i > 0 && stream[i - 1] == 0 ? i - 1 : i);
    }
    std::vector<Bytes> units;
    for (std::size_t n = 0; n < starts.size(); ++n) {
        const std::size_t end = n + 1 < starts.size() ? starts[n + 1] : stream.size();
        units.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(starts[n]),
                           stream.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return units;
}

void expectClipDecodes(const char* name, VideoCodec codec, int y, int u, int v) {
    const auto units = accessUnits(readClip(name), codec == VideoCodec::Hevc);
    ASSERT_EQ(units.size(), 6u) << name;

    auto decoder = media::VideoDecoder::create({codec, nullptr});
    ASSERT_TRUE(decoder.ok()) << decoder.error().message;

    int frames = 0;
    for (const Bytes& unit : units) {
        const auto decoded = (*decoder)->decode(unit);
        ASSERT_TRUE(decoded.ok()) << decoded.error().message;
        if (!decoded->has_value()) continue;
        const media::VideoFrame& frame = **decoded;
        ++frames;
        EXPECT_EQ(frame.sequence, static_cast<std::uint64_t>(frames));
        EXPECT_EQ(frame.width, 128);
        EXPECT_EQ(frame.height, 72);
        ASSERT_EQ(frame.layout, media::PixelLayout::I420);
        EXPECT_FALSE(frame.color.bt601);
        EXPECT_FALSE(frame.color.fullRange);
        ASSERT_NE(frame.owner, nullptr);
        // Centre of each plane; lossy coding may be off by a step or two.
        EXPECT_NEAR(frame.planes[0][36 * frame.strides[0] + 64], y, 3);
        EXPECT_NEAR(frame.planes[1][18 * frame.strides[1] + 32], u, 3);
        EXPECT_NEAR(frame.planes[2][18 * frame.strides[2] + 32], v, 3);
    }
    // No reordering: every frame that went in came out.
    EXPECT_EQ(frames, 6) << name;
    EXPECT_FALSE((*decoder)->hardware());
}

struct FakeAudioOutput final : media::IAudioOutput {
    bool available = true;
    bool open = false;
    int sampleRate = 0;
    int channels = 0;
    std::chrono::milliseconds delay{0};
    std::vector<std::int16_t> samples;
    int stops = 0;

    bool start(int rate, int channelCount, std::chrono::milliseconds reserve) override {
        if (!available) return false;
        open = true;
        sampleRate = rate;
        channels = channelCount;
        delay = reserve;
        return true;
    }
    void write(std::span<const std::int16_t> interleaved) override {
        samples.insert(samples.end(), interleaved.begin(), interleaved.end());
    }
    void stop() override {
        if (open) ++stops;
        open = false;
    }
    bool running() const override { return open; }
};

/// Ten milliseconds of a 440 Hz tone, stereo, as an Opus packet.
Bytes encodeTone() {
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    std::vector<std::int16_t> pcm(480 * 2);
    for (int i = 0; i < 480; ++i) {
        const auto value = static_cast<std::int16_t>(12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
        pcm[static_cast<std::size_t>(i) * 2] = value;
        pcm[static_cast<std::size_t>(i) * 2 + 1] = value;
    }
    Bytes packet(1000);
    const int size = opus_encode(encoder, pcm.data(), 480, packet.data(), static_cast<opus_int32>(packet.size()));
    opus_encoder_destroy(encoder);
    packet.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    return packet;
}

} // namespace

TEST(ColorMatrix, MapsLimitedRangeBt709) {
    const media::ColorMatrix m = media::yuvToRgb({false, false});
    const auto rgb = [&](int y, int u, int v) {
        std::array<float, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = m.rows[i][0] * (static_cast<float>(y) / 255.0f) + m.rows[i][1] * (static_cast<float>(u) / 255.0f) +
                     m.rows[i][2] * (static_cast<float>(v) / 255.0f) + m.rows[i][3];
        }
        return out;
    };
    const auto black = rgb(16, 128, 128);
    const auto white = rgb(235, 128, 128);
    const auto red = rgb(63, 102, 240); // pure red in BT.709
    for (float c : black) EXPECT_NEAR(c, 0.0f, 0.005f);
    for (float c : white) EXPECT_NEAR(c, 1.0f, 0.005f);
    EXPECT_NEAR(red[0], 1.0f, 0.02f);
    EXPECT_NEAR(red[1], 0.0f, 0.02f);
    EXPECT_NEAR(red[2], 0.0f, 0.02f);
}

TEST(ColorMatrix, KnowsFullRangeAndBt601) {
    const media::ColorMatrix full = media::yuvToRgb({true, true});
    // Full range: luma passes through unscaled, chroma is centred on one half.
    EXPECT_FLOAT_EQ(full.rows[0][0], 1.0f);
    EXPECT_NEAR(full.rows[0][2], 1.402f, 0.001f); // the well-known BT.601 factor for red
    EXPECT_NEAR(full.rows[2][1], 1.772f, 0.001f);
    EXPECT_NEAR(full.rows[0][3], -0.701f, 0.001f);

    const media::ColorMatrix hd = media::yuvToRgb({false, true});
    EXPECT_NEAR(hd.rows[0][2], 1.5748f, 0.001f); // and for BT.709
}

TEST(VideoDecoder, DecodesH264) { expectClipDecodes("clip.h264", VideoCodec::H264, 88, 118, 196); }

TEST(VideoDecoder, DecodesHevc) { expectClipDecodes("clip.h265", VideoCodec::Hevc, 91, 180, 93); }

TEST(VideoDecoder, SurvivesBrokenInput) {
    auto decoder = media::VideoDecoder::create({VideoCodec::Hevc, nullptr});
    ASSERT_TRUE(decoder.ok());
    EXPECT_FALSE((*decoder)->decode({}).ok());

    Bytes noise(4000);
    for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<std::uint8_t>(i * 131 + 7);
    const auto garbage = (*decoder)->decode(noise);
    EXPECT_TRUE(!garbage.ok() || !garbage->has_value());

    // A damaged frame in the middle must not take the decoder down for good.
    auto units = accessUnits(readClip("clip.h265"), true);
    ASSERT_EQ(units.size(), 6u);
    for (std::size_t i = 40; i < units[1].size(); ++i) units[1][i] ^= 0x5a;
    int frames = 0;
    for (const Bytes& unit : units) {
        const auto decoded = (*decoder)->decode(unit);
        if (decoded.ok() && decoded->has_value()) ++frames;
    }
    EXPECT_GE(frames, 3); // the key frame at index 3 starts a clean run
}

TEST(AudioDecoder, DecodesAnOpusPacket) {
    const Bytes packet = encodeTone();
    ASSERT_FALSE(packet.empty());

    auto decoder = media::AudioDecoder::create(48000, 2);
    ASSERT_TRUE(decoder.ok()) << decoder.error().message;
    // The codec needs a few packets to settle; the last one must carry the tone.
    std::span<const std::int16_t> pcm;
    for (int i = 0; i < 4; ++i) {
        const auto decoded = (*decoder)->decode(packet);
        ASSERT_TRUE(decoded.ok()) << decoded.error().message;
        pcm = *decoded;
    }
    ASSERT_EQ(pcm.size(), 480u * 2u);
    int peak = 0;
    for (std::int16_t sample : pcm) peak = std::max(peak, std::abs(static_cast<int>(sample)));
    EXPECT_GT(peak, 4000);
    EXPECT_LT(peak, 20000);

    EXPECT_FALSE((*decoder)->decode({}).ok());
    const Bytes junk = {0xff, 0xff, 0xff};
    (void)(*decoder)->decode(junk); // may fail, must not crash
    EXPECT_FALSE(media::AudioDecoder::create(48000, 6).ok());
}

TEST(AudioBuffer, WaitsForItsReserveBeforePlaying) {
    media::AudioBuffer buffer(48000, 2, 20ms, 100ms); // 960 frames reserve
    std::vector<std::int16_t> packet(480 * 2, 1000);
    std::vector<std::int16_t> out(480 * 2, -1);

    buffer.push(packet);
    EXPECT_EQ(buffer.buffered(), 10ms);
    EXPECT_EQ(buffer.pop(out), 0u); // not enough yet: silence
    EXPECT_EQ(out[0], 0);

    buffer.push(packet);
    EXPECT_EQ(buffer.pop(out), 480u);
    EXPECT_EQ(out[0], 1000);
    EXPECT_EQ(buffer.buffered(), 10ms);
    EXPECT_EQ(buffer.underruns(), 0u);
}

TEST(AudioBuffer, RefillsAfterRunningDry) {
    media::AudioBuffer buffer(48000, 2, 10ms, 100ms);
    std::vector<std::int16_t> packet(480 * 2, 500);
    std::vector<std::int16_t> out(480 * 2);
    buffer.push(packet);
    EXPECT_EQ(buffer.pop(out), 480u);

    // Nothing left: silence, counted once, and playback pauses until the
    // reserve is back instead of stuttering through every late packet.
    EXPECT_EQ(buffer.pop(out), 0u);
    EXPECT_EQ(buffer.underruns(), 1u);
    std::vector<std::int16_t> half(240 * 2, 500);
    buffer.push(half);
    EXPECT_EQ(buffer.pop(out), 0u);
    EXPECT_EQ(buffer.underruns(), 1u);
    buffer.push(half);
    EXPECT_EQ(buffer.pop(out), 480u);
}

TEST(AudioBuffer, DropsOldAudioInsteadOfFallingBehind) {
    media::AudioBuffer buffer(48000, 2, 20ms, 60ms);
    std::vector<std::int16_t> packet(480 * 2);
    for (int i = 0; i < 10; ++i) { // 100 ms arrive at once
        std::fill(packet.begin(), packet.end(), static_cast<std::int16_t>(i));
        buffer.push(packet);
    }
    EXPECT_LE(buffer.buffered(), 60ms);
    EXPECT_GE(buffer.buffered(), 20ms);
    EXPECT_GT(buffer.droppedFrames(), 0u);

    // What plays next is recent audio, not the oldest packet.
    std::vector<std::int16_t> out(480 * 2);
    ASSERT_EQ(buffer.pop(out), 480u);
    EXPECT_GE(out[0], 5);
}

TEST(Haptics, UpsamplesOntoTheActuatorChannels) {
    media::HapticsUpsampler upsampler;
    std::vector<std::int16_t> packet(30 * 2);
    for (std::size_t i = 0; i < 30; ++i) {
        packet[i * 2] = 1600;
        packet[i * 2 + 1] = -3200;
    }
    const auto out = upsampler.convert(packet);
    ASSERT_EQ(out.size(), 30u * 16u * 4u); // ten milliseconds at 48 kHz, four channels

    for (std::size_t frame = 0; frame < out.size() / 4; ++frame) {
        EXPECT_EQ(out[frame * 4], 0); // the controller's speaker stays silent
        EXPECT_EQ(out[frame * 4 + 1], 0);
    }
    // A ramp from rest to the first sample, then the level holds.
    EXPECT_EQ(out[2], 100);
    EXPECT_EQ(out[3], -200);
    EXPECT_EQ(out[15 * 4 + 2], 1600);
    EXPECT_EQ(out[200 * 4 + 2], 1600);
    EXPECT_EQ(out[200 * 4 + 3], -3200);

    // The next packet continues where this one ended.
    const auto next = upsampler.convert(packet);
    EXPECT_EQ(next[2], 1600);
}

TEST(Haptics, TurnsLoudnessIntoRumble) {
    std::vector<std::int16_t> silence(60, 0);
    EXPECT_EQ(media::hapticsToRumble(silence), (media::RumbleLevel{0, 0}));

    std::vector<std::int16_t> noise(60, 30); // below the floor: idle hiss
    EXPECT_EQ(media::hapticsToRumble(noise), (media::RumbleLevel{0, 0}));

    std::vector<std::int16_t> leftOnly(60, 0);
    for (std::size_t i = 0; i < 30; ++i) leftOnly[i * 2] = (i % 2) ? 8000 : -8000;
    const media::RumbleLevel left = media::hapticsToRumble(leftOnly);
    EXPECT_EQ(left.low, 62); // 2 * 8000 of 65535, scaled to 255
    EXPECT_EQ(left.high, 0);

    std::vector<std::int16_t> loud(60, 32767);
    EXPECT_EQ(media::hapticsToRumble(loud), (media::RumbleLevel{255, 255}));
    EXPECT_EQ(media::hapticsToRumble({}), (media::RumbleLevel{0, 0}));
}

TEST(MediaPipeline, DecodesTheStreamAndFeedsTheOutputs) {
    FakeAudioOutput audio;
    FakeAudioOutput haptics;
    media::MediaPipeline pipeline({nullptr, &audio, &haptics});
    pipeline.setAudioDelay(60ms);

    StreamSettings settings;
    settings.codec = VideoCodec::H264;
    pipeline.begin(settings);
    EXPECT_FALSE(pipeline.takeFrame().has_value());
    EXPECT_TRUE(haptics.open);
    EXPECT_EQ(haptics.sampleRate, 48000);
    EXPECT_EQ(haptics.channels, 4);

    // Video: the renderer always gets the newest picture.
    const auto units = accessUnits(readClip("clip.h264"), false);
    for (const Bytes& unit : units) {
        EXPECT_TRUE(pipeline.onVideo(unit, 0, false));
        ASSERT_TRUE(pipeline.waitUntilIdle(5000ms)); // one at a time, so none is dropped
    }
    const auto frame = pipeline.takeFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->sequence, 6u);
    EXPECT_FALSE(pipeline.takeFrame().has_value()); // handed out once
    auto stats = pipeline.stats();
    EXPECT_EQ(stats.decodedFrames, 6u);
    EXPECT_EQ(stats.failedFrames, 0u);
    EXPECT_FALSE(stats.hardware);
    EXPECT_EQ(stats.decoder, "H.264, CPU");
    EXPECT_GT(stats.decodeMs, 0.0);

    // A frame that cannot be decoded is reported with the next one, so the
    // console repairs the stream.
    Bytes broken = units[1];
    for (std::size_t i = 8; i < broken.size(); ++i) broken[i] = 0xff;
    EXPECT_TRUE(pipeline.onVideo(broken, 0, false));
    ASSERT_TRUE(pipeline.waitUntilIdle(5000ms));
    if (pipeline.stats().failedFrames > 0) {
        // The unit after a lost picture is refused and not decoded either.
        const auto before = pipeline.stats();
        EXPECT_FALSE(pipeline.onVideo(units[2], 0, false));
        ASSERT_TRUE(pipeline.waitUntilIdle(5000ms));
        EXPECT_EQ(pipeline.stats().decodedFrames + pipeline.stats().failedFrames,
                  before.decodedFrames + before.failedFrames);
        EXPECT_TRUE(pipeline.onVideo(units[3], 0, false)); // the key frame gets through again
        ASSERT_TRUE(pipeline.waitUntilIdle(5000ms));
    }

    // Audio: opened with the format the console announces and the chosen reserve.
    pipeline.onAudio(encodeTone()); // before the format is known: ignored
    EXPECT_TRUE(audio.samples.empty());
    pipeline.onAudioFormat({2, 16, 48000, 480});
    EXPECT_TRUE(audio.open);
    EXPECT_EQ(audio.delay, 60ms);
    pipeline.onAudio(encodeTone());
    EXPECT_EQ(audio.samples.size(), 480u * 2u);
    EXPECT_TRUE(pipeline.stats().audioPlaying);

    // Haptics go to the DualSense's sound device.
    std::vector<std::int16_t> packet(60, 2000);
    EXPECT_TRUE(pipeline.onHaptics(packet));
    EXPECT_EQ(haptics.samples.size(), 30u * 16u * 4u);

    pipeline.end();
    EXPECT_FALSE(audio.open);
    EXPECT_FALSE(haptics.open);
    EXPECT_FALSE(pipeline.takeFrame().has_value());
    EXPECT_FALSE(pipeline.onHaptics(packet)); // nothing plays after the end
    EXPECT_FALSE(pipeline.onVideo(units[0], 0, false));
}

TEST(DecodeQueue, TakesABurstButNotABacklog) {
    using Queue = media::DecodeQueue;
    Queue queue({250ms, 6});
    const Queue::Clock::time_point start{};
    std::size_t dropped = 0;

    // Five frames at once, as Wi-Fi delivers them after a pause: all taken.
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(queue.push(Bytes{static_cast<std::uint8_t>(i)}, start + 1ms, dropped));
    EXPECT_EQ(queue.size(), 5u);
    EXPECT_EQ(dropped, 0u);
    EXPECT_EQ((*queue.pop())[0], 0); // oldest first
    EXPECT_EQ((*queue.pop())[0], 1);

    // Still within the time the decoder is given: taken.
    EXPECT_TRUE(queue.push(Bytes{9}, start + 200ms, dropped));

    // The oldest has now waited too long. The decoder is behind: everything
    // goes, the new unit included, and the caller learns how much was lost.
    EXPECT_FALSE(queue.push(Bytes{10}, start + 300ms, dropped));
    EXPECT_EQ(dropped, 5u);
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.pop().has_value());

    // After that it starts afresh.
    EXPECT_TRUE(queue.push(Bytes{11}, start + 301ms, dropped));
    EXPECT_EQ(dropped, 0u);

    // The cap on memory works the same way.
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(queue.push(Bytes{1}, start + 302ms, dropped));
    EXPECT_FALSE(queue.push(Bytes{1}, start + 302ms, dropped));
    EXPECT_EQ(dropped, 7u);
}

TEST(MediaPipeline, DecodesABurstWithoutDroppingAnything) {
    media::MediaPipeline pipeline({});
    StreamSettings settings;
    settings.codec = VideoCodec::H264;
    pipeline.begin(settings);

    // Thirty frames handed over in one go. More than eight used to be thrown
    // away, which broke the picture until the next key frame.
    const auto units = accessUnits(readClip("clip.h264"), false);
    for (int round = 0; round < 5; ++round) {
        for (const Bytes& unit : units) EXPECT_TRUE(pipeline.onVideo(unit, 0, false));
    }
    ASSERT_TRUE(pipeline.waitUntilIdle(10'000ms));
    const auto stats = pipeline.stats();
    EXPECT_EQ(stats.droppedFrames, 0u);
    EXPECT_EQ(stats.decodedFrames + stats.failedFrames, 30u);
    EXPECT_GE(stats.decodedFrames, 6u);
    // The slowest picture is reported next to the average, and is never below it.
    EXPECT_GT(stats.decodeMaxMs, 0.0);
    EXPECT_GE(stats.decodeMaxMs, stats.decodeMs * 0.999);
    pipeline.end();
}

TEST(MediaPipeline, HandsOutEveryPictureWithItsTimes) {
    media::MediaPipeline pipeline({});
    StreamSettings settings;
    settings.codec = VideoCodec::H264;
    pipeline.begin(settings);
    const auto before = std::chrono::steady_clock::now();
    for (const Bytes& unit : accessUnits(readClip("clip.h264"), false)) EXPECT_TRUE(pipeline.onVideo(unit, 0, false));
    ASSERT_TRUE(pipeline.waitUntilIdle(10'000ms));
    const auto after = std::chrono::steady_clock::now();

    // Several pictures were decoded before anyone fetched. All that are kept
    // come out, oldest first, each with when it arrived and when it was done.
    const std::vector<media::VideoFrame> frames = pipeline.takeFrames();
    ASSERT_GE(frames.size(), 2u);
    ASSERT_LE(frames.size(), 4u);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_GE(frames[i].receivedAt, before);
        EXPECT_GE(frames[i].decodedAt, frames[i].receivedAt);
        EXPECT_LE(frames[i].decodedAt, after);
        if (i > 0) EXPECT_GT(frames[i].sequence, frames[i - 1].sequence);
    }
    EXPECT_TRUE(pipeline.takeFrames().empty()); // handed out once
    pipeline.end();
}

TEST(MediaPipeline, WorksWithoutSoundDevices) {
    FakeAudioOutput audio;
    audio.available = false;
    FakeAudioOutput haptics;
    haptics.available = false;
    media::MediaPipeline pipeline({nullptr, &audio, &haptics});
    StreamSettings settings;
    settings.codec = VideoCodec::Hevc;
    pipeline.begin(settings);

    pipeline.onAudioFormat({2, 16, 48000, 480});
    pipeline.onAudio(encodeTone());
    EXPECT_TRUE(audio.samples.empty());
    EXPECT_FALSE(pipeline.stats().audioPlaying);

    // No DualSense sound device: the caller falls back to the rumble motors.
    std::vector<std::int16_t> packet(60, 2000);
    EXPECT_FALSE(pipeline.onHaptics(packet));

    for (const Bytes& unit : accessUnits(readClip("clip.h265"), true)) {
        EXPECT_TRUE(pipeline.onVideo(unit, 0, false));
        ASSERT_TRUE(pipeline.waitUntilIdle(5000ms));
    }
    EXPECT_TRUE(pipeline.takeFrame().has_value());

    // And entirely without outputs.
    media::MediaPipeline bare({});
    bare.begin(settings);
    bare.onAudioFormat({2, 16, 48000, 480});
    bare.onAudio(encodeTone());
    EXPECT_FALSE(bare.onHaptics(packet));
}

// ---- Frame pacing -----------------------------------------------------------

TEST(InputRateDetector, FollowsWhatArrivesButNotASingleBadSecond) {
    media::InputRateDetector detector(60);
    EXPECT_EQ(detector.fps(), 60);
    // A 60 fps stream on Wi-Fi: one bad second, then fine again.
    detector.addSample(59.8);
    detector.addSample(33.0);
    detector.addSample(34.0);
    detector.addSample(60.1);
    EXPECT_EQ(detector.fps(), 60);
    // The console really sends 30: three seconds in a row settle it.
    detector.addSample(30.0);
    detector.addSample(29.9);
    EXPECT_EQ(detector.fps(), 60);
    detector.addSample(30.1);
    EXPECT_EQ(detector.fps(), 30);
    // A stream that stands still says nothing, and neither does the grey zone.
    detector.addSample(0.0);
    detector.addSample(2.0);
    detector.addSample(42.0);
    detector.addSample(0.0);
    EXPECT_EQ(detector.fps(), 30);
    detector.addSample(58.0);
    detector.addSample(61.0);
    detector.addSample(60.0);
    EXPECT_EQ(detector.fps(), 60);
    detector.reset(30);
    EXPECT_EQ(detector.fps(), 30);
}

TEST(FrameGenerationFactor, AutomaticDoublesThirtyAndLeavesSixtyAlone) {
    EXPECT_EQ(media::frameGenerationFactorFor(0, 30), 2);
    EXPECT_EQ(media::frameGenerationFactorFor(0, 60), 1);
    EXPECT_EQ(media::frameGenerationFactorFor(1, 30), 1);
    EXPECT_EQ(media::frameGenerationFactorFor(3, 30), 3);
    EXPECT_EQ(media::frameGenerationFactorFor(4, 60), 4);
    EXPECT_EQ(media::frameGenerationFactorFor(9, 30), 4);
}

TEST(FramePacer, ShowsEveryPictureAtOnceWithoutGeneration) {
    using Clock = media::FramePacer::Clock;
    media::FramePacer pacer;
    pacer.configure(1, 33'333us);
    const Clock::time_point start{};
    EXPECT_FALSE(pacer.choose(start).valid);
    EXPECT_EQ(pacer.addedDelay(), Clock::duration::zero());

    pacer.onFrame(1, start);
    media::FrameChoice choice = pacer.choose(start);
    ASSERT_TRUE(choice.valid);
    EXPECT_EQ(choice.base, 1u);
    EXPECT_EQ(choice.step, 0);
    pacer.onFrame(2, start + 33ms);
    choice = pacer.choose(start + 33ms);
    EXPECT_EQ(choice.base, 2u);
    EXPECT_EQ(choice.step, 0);
}

TEST(FramePacer, PutsTheGeneratedPictureFirstAndTheRealOneHalfAPeriodLater) {
    using Clock = media::FramePacer::Clock;
    media::FramePacer pacer;
    const auto period = 33'333us;
    pacer.configure(2, period);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(pacer.addedDelay()).count(), 16'666);

    const Clock::time_point start{};
    // The very first picture has nothing before it and is shown as it is.
    pacer.onFrame(1, start);
    media::FrameChoice choice = pacer.choose(start + 1ms);
    ASSERT_TRUE(choice.valid);
    EXPECT_EQ(choice.base, 1u);
    EXPECT_EQ(choice.step, 0);

    // The second arrives one period later. Right then the picture between
    // the two is due ...
    pacer.onFrame(2, start + period);
    choice = pacer.choose(start + period);
    EXPECT_EQ(choice.base, 1u);
    EXPECT_EQ(choice.next, 2u);
    EXPECT_EQ(choice.step, 1);
    EXPECT_EQ(choice.steps, 2);
    choice = pacer.choose(start + period + 10ms);
    EXPECT_EQ(choice.step, 1);
    // ... and half a period later the second picture itself.
    choice = pacer.choose(start + period + 17ms);
    EXPECT_EQ(choice.base, 2u);
    EXPECT_EQ(choice.step, 0);
    // It stays until the third arrives, however late that is.
    choice = pacer.choose(start + period + 200ms);
    EXPECT_EQ(choice.base, 2u);
    EXPECT_EQ(choice.step, 0);
}

TEST(FramePacer, WalksThroughAllStepsAtHigherFactors) {
    using Clock = media::FramePacer::Clock;
    media::FramePacer pacer;
    const auto period = 40ms; // easy numbers: four steps of 10 ms
    pacer.configure(4, period);
    EXPECT_EQ(pacer.addedDelay(), Clock::duration(30ms));
    const Clock::time_point start{};
    pacer.onFrame(10, start);
    pacer.onFrame(11, start + period);
    const Clock::time_point arrival = start + period;
    EXPECT_EQ(pacer.choose(arrival + 1ms).step, 1);
    EXPECT_EQ(pacer.choose(arrival + 11ms).step, 2);
    EXPECT_EQ(pacer.choose(arrival + 21ms).step, 3);
    const media::FrameChoice last = pacer.choose(arrival + 31ms);
    EXPECT_EQ(last.base, 11u);
    EXPECT_EQ(last.step, 0);
}

TEST(FramePacer, GivesAPictureThatArrivesRightBehindAnotherItsOwnTime) {
    using Clock = media::FramePacer::Clock;
    media::FramePacer pacer;
    const auto period = 33'333us;
    pacer.configure(2, period);
    const Clock::time_point start{};
    pacer.onFrame(1, start);
    pacer.onFrame(2, start + period);
    // The third comes 2 ms after the second: Wi-Fi delivered both together.
    pacer.onFrame(3, start + period + 2ms);
    // Picture 1 is gone, so nothing older than picture 2 is shown any more ...
    media::FrameChoice choice = pacer.choose(start + period + 3ms);
    ASSERT_TRUE(choice.valid);
    EXPECT_EQ(choice.base, 2u);
    EXPECT_EQ(choice.step, 0);
    // ... but picture 3 is not squeezed in 2 ms later. It gets nearly a whole
    // period, with the picture between the two halfway.
    choice = pacer.choose(start + period + 2ms + 17ms);
    EXPECT_EQ(choice.base, 2u);
    choice = pacer.choose(start + 2 * period + 4ms);
    EXPECT_EQ(choice.base, 2u);
    EXPECT_EQ(choice.next, 3u);
    EXPECT_EQ(choice.step, 1);
    choice = pacer.choose(start + 2 * period + 20ms);
    EXPECT_EQ(choice.base, 3u);
    EXPECT_EQ(choice.step, 0);

    // Switching generation off mid-stream shows the newest picture at once.
    pacer.configure(1, period);
    choice = pacer.choose(start + period + 3ms);
    EXPECT_EQ(choice.base, 3u);
    EXPECT_EQ(choice.step, 0);
}

TEST(FramePacer, EvensOutPicturesThatArriveUnevenly) {
    using Clock = media::FramePacer::Clock;
    media::FramePacer pacer;
    const auto period = 33'333us;
    pacer.configure(2, period);
    const Clock::time_point start{};
    // Every third picture is 14 ms late, the one after it on time again.
    Clock::time_point before{};
    Clock::duration shortest = Clock::duration::max();
    Clock::duration longest = Clock::duration::zero();
    Clock::duration lateness{};
    for (int index = 0; index < 90; ++index) {
        const Clock::time_point arrival = start + index * period + (index % 3 == 1 ? 14ms : 0ms);
        pacer.onFrame(static_cast<std::uint64_t>(index + 1), arrival);
        const Clock::time_point due = pacer.newestDue();
        EXPECT_GE(due, arrival + pacer.addedDelay()); // never before it can be shown
        if (index > 0) {
            shortest = std::min(shortest, due - before);
            longest = std::max(longest, due - before);
        }
        lateness = due - (arrival + pacer.addedDelay());
        before = due;
    }
    // Shown as they arrive, the distances would swing between 19 and 47 ms.
    EXPECT_GE(shortest, Clock::duration(32ms));
    EXPECT_LE(longest, Clock::duration(48ms));
    // And the display does not drift ever further behind.
    EXPECT_LE(lateness, Clock::duration(15ms));

    // After a long pause it does not trail behind; it starts afresh.
    const Clock::time_point later = start + 200 * period;
    pacer.onFrame(500, later);
    EXPECT_EQ(pacer.newestDue(), later + pacer.addedDelay());
}

TEST(FramePacer, KeepsItsRhythmWhenStepsFallRightOnTheRefreshes) {
    using Clock = media::FramePacer::Clock;
    // A 30 fps stream on a 60 Hz screen: the pictures are taken over at a
    // refresh, and every step then falls exactly on a later refresh, give or
    // take what the loop wobbles. Without slack that wobble decides whether a
    // picture goes out now or one refresh late, and pictures get skipped.
    const auto refresh = 16'667us;
    const auto period = 33'333us;
    const auto wobble = [](int index) { return std::chrono::microseconds((index * 7919) % 601 - 300); };

    for (const bool withSlack : {true, false}) {
        media::FramePacer pacer;
        pacer.configure(2, period);
        const Clock::time_point start{};
        std::uint64_t arrived = 0;
        int shown = 0;
        int real = 0;
        int computed = 0;
        media::FrameChoice last{};
        // Two seconds; the first half second is left out of the count.
        for (int index = 0; index < 120; ++index) {
            const Clock::time_point now = start + index * refresh + wobble(index);
            if (index % 2 == 0) pacer.onFrame(++arrived, now); // a picture arrived just before this refresh
            const media::FrameChoice choice =
                pacer.choose(now, withSlack ? Clock::duration(refresh / 2) : Clock::duration::zero());
            if (index >= 30 && !(choice == last)) {
                ++shown;
                (choice.step == 0 ? real : computed) += 1;
            }
            last = choice;
        }
        if (withSlack) {
            // Every refresh shows something new, real and computed pictures in turn.
            EXPECT_EQ(shown, 90);
            EXPECT_EQ(real, 45);
            EXPECT_EQ(computed, 45);
        } else {
            // What 0.6.0 did: clearly fewer than one new picture per refresh.
            EXPECT_LT(shown, 85);
        }
    }
}
