// The Linux recorder against the FFmpeg of this machine: write a file, read
// it back with libavformat and look into the first picture.
#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "linux/FfmpegRecorder.hpp"

using namespace veloplay;

namespace {

constexpr int kRecWidth = 1920;
constexpr int kRecHeight = 1080;
constexpr int kRecMarker = 160; // a red square in the top left corner, the rest dark grey

std::vector<std::uint8_t> recMarkedPicture(int shade) {
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(kRecWidth) * kRecHeight * 4);
    for (int y = 0; y < kRecHeight; ++y) {
        for (int x = 0; x < kRecWidth; ++x) {
            std::uint8_t* pixel = &bgra[(static_cast<std::size_t>(y) * kRecWidth + x) * 4];
            const bool marker = x < kRecMarker && y < kRecMarker;
            pixel[0] = marker ? 20 : static_cast<std::uint8_t>(shade);
            pixel[1] = marker ? 20 : static_cast<std::uint8_t>(shade);
            pixel[2] = marker ? 235 : static_cast<std::uint8_t>(shade);
            pixel[3] = 255;
        }
    }
    return bgra;
}

/// Its own name per test and process: other checkouts may run the same tests at the same time.
std::filesystem::path recTempFile(const std::string& what) {
    return std::filesystem::temp_directory_path() /
           ("veloplay-linux-recording-" + what + "-" + std::to_string(::getpid()) + ".mp4");
}

struct RecReadBack {
    std::string error;
    int streams = 0;
    int videoPackets = 0;
    int audioPackets = 0;
    double seconds = 0.0;      // of the whole file, the longer of picture and sound
    double videoSeconds = 0.0; // to the end of the last picture
    std::string videoCodec;
    std::string audioCodec;
    int audioRate = 0;
    int audioChannels = 0;
    bool bt709 = false;
    // Of the first decoded picture, in 8-bit luma and chroma: the marker, below it, the bottom left.
    bool looked = false;
    double markerY = 0, markerV = 0, belowY = 0, bottomLeftY = 0;
};

double recAverage(const AVFrame* frame, int plane, int x0, int y0, int size) {
    double sum = 0;
    for (int y = y0; y < y0 + size; ++y) {
        for (int x = x0; x < x0 + size; ++x) sum += frame->data[plane][static_cast<std::ptrdiff_t>(y) * frame->linesize[plane] + x];
    }
    return sum / (size * size);
}

RecReadBack recReadFile(const std::filesystem::path& file) {
    RecReadBack back;
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0) {
        back.error = "cannot open the file";
        return back;
    }
    if (avformat_find_stream_info(format, nullptr) < 0) back.error = "no stream info";
    back.streams = static_cast<int>(format->nb_streams);
    if (format->duration > 0) back.seconds = static_cast<double>(format->duration) / AV_TIME_BASE;
    int videoIndex = -1;
    AVCodecContext* decoder = nullptr;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVCodecParameters* parameters = format->streams[i]->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && videoIndex < 0) {
            videoIndex = static_cast<int>(i);
            back.videoCodec = avcodec_get_name(parameters->codec_id);
            back.bt709 = parameters->color_space == AVCOL_SPC_BT709;
            if (const AVCodec* codec = avcodec_find_decoder(parameters->codec_id)) {
                decoder = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(decoder, parameters);
                if (avcodec_open2(decoder, codec, nullptr) < 0) avcodec_free_context(&decoder);
            }
        } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            back.audioCodec = avcodec_get_name(parameters->codec_id);
            back.audioRate = parameters->sample_rate;
            back.audioChannels = parameters->ch_layout.nb_channels;
        }
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto look = [&] {
        if (back.looked || !decoder || avcodec_receive_frame(decoder, frame) < 0) return;
        back.looked = true;
        back.markerY = recAverage(frame, 0, 40, 40, 60);
        back.markerV = recAverage(frame, 2, 20, 20, 30);
        back.belowY = recAverage(frame, 0, 40, 400, 60);
        back.bottomLeftY = recAverage(frame, 0, 40, frame->height - 120, 60);
    };
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == videoIndex) {
            ++back.videoPackets;
            if (packet->pts != AV_NOPTS_VALUE) {
                const double end = static_cast<double>(packet->pts + packet->duration) *
                                   av_q2d(format->streams[videoIndex]->time_base);
                back.videoSeconds = std::max(back.videoSeconds, end);
            }
            if (decoder && !back.looked && avcodec_send_packet(decoder, packet) >= 0) look();
        } else {
            ++back.audioPackets;
        }
        av_packet_unref(packet);
    }
    if (decoder && !back.looked && avcodec_send_packet(decoder, nullptr) >= 0) look();
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return back;
}

/// Hands a picture over, waiting while the encoder is behind, so every picture goes in and the numbers are exact.
void recAddWaiting(linuxos::VideoRecorder& recorder, const std::vector<std::uint8_t>& picture, media::ShownKind kind,
                   std::chrono::steady_clock::time_point at) {
    for (int attempt = 0; attempt < 4000; ++attempt) {
        if (recorder.addPicture(std::vector<std::uint8_t>(picture), kind, at)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ADD_FAILURE() << "the recorder never took the picture";
}

} // namespace

TEST(LinuxVideoRecorder, WritesAFileThatPlaysWithItsPicturesInOrder) {
    std::string why;
    if (!linuxos::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    std::printf("Recorder: pictures with %s\n", linuxos::VideoRecorder::firstEncoder().c_str());
    const auto file = recTempFile("test");
    std::filesystem::remove(file);

    linuxos::VideoRecorder recorder;
    const auto began = std::chrono::steady_clock::now();
    const auto opened = recorder.start(file, kRecWidth, kRecHeight, 60);
    ASSERT_TRUE(opened) << opened.error().message;
    const double openTook = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    EXPECT_TRUE(recorder.running());
    EXPECT_EQ(recorder.width(), kRecWidth);
    EXPECT_FALSE(recorder.encoder().empty());
    const auto start = std::chrono::steady_clock::now();
    constexpr int kPictures = 90;
    std::vector<std::vector<std::uint8_t>> shades;
    for (int s = 0; s < 20; ++s) shades.push_back(recMarkedPicture(40 + s));
    for (int i = 0; i < kPictures; ++i) {
        const auto kind = i % 2 == 0 ? media::ShownKind::Real : media::ShownKind::Computed;
        recAddWaiting(recorder, shades[static_cast<std::size_t>(i % 20)], kind, start + std::chrono::microseconds(16'667LL * i));
        // Sound for this picture: a quiet tone, 800 samples per channel at 48 kHz.
        std::vector<std::int16_t> tone(1600);
        for (int s = 0; s < 800; ++s) {
            const auto v = static_cast<std::int16_t>(3000 * std::sin((i * 800 + s) * 2 * 3.14159265 * 440 / 48000));
            tone[static_cast<std::size_t>(s) * 2] = v;
            tone[static_cast<std::size_t>(s) * 2 + 1] = v;
        }
        recorder.addSound(tone, 48'000, 2);
        recorder.addSound(tone, 44'100, 2); // another rate is left out, not mixed in
    }
    const auto stopBegan = std::chrono::steady_clock::now();
    const auto summary = recorder.stop();
    ASSERT_TRUE(summary) << summary.error().message;
    const auto now = std::chrono::steady_clock::now();
    std::printf("Recording 1080p with %s: opened in %.3f s, %zu pictures written, %zu left out, %.2f s for %d pictures "
                "(%.2f s to close)\n",
                recorder.encoder().c_str(), openTook, summary->pictures, summary->dropped,
                std::chrono::duration<double>(now - start).count(), kPictures,
                std::chrono::duration<double>(now - stopBegan).count());
    EXPECT_FALSE(recorder.running());
    EXPECT_EQ(summary->pictures, static_cast<std::size_t>(kPictures));
    EXPECT_EQ(summary->pictures, summary->log.size());
    EXPECT_EQ(summary->log.count(media::ShownKind::Computed), static_cast<std::size_t>(kPictures / 2));
    EXPECT_TRUE(summary->sound);
    EXPECT_EQ(summary->file, file);

    const RecReadBack back = recReadFile(file);
    ASSERT_TRUE(back.error.empty()) << back.error;
    std::printf("Read back: %d streams, %s %d packets to %.3f s, %s %d Hz %d channels %d packets, %.2f s; marker Y %.0f V %.0f, "
                "below %.0f, bottom left %.0f\n",
                back.streams, back.videoCodec.c_str(), back.videoPackets, back.videoSeconds, back.audioCodec.c_str(), back.audioRate,
                back.audioChannels, back.audioPackets, back.seconds, back.markerY, back.markerV, back.belowY,
                back.bottomLeftY);
    EXPECT_EQ(back.streams, 2);
    EXPECT_EQ(back.audioCodec, "aac");
    EXPECT_EQ(back.audioRate, 48'000);
    EXPECT_EQ(back.audioChannels, 2);
    EXPECT_GT(back.audioPackets, 0);
    // The frame log describes the file: one letter per picture in it.
    EXPECT_EQ(static_cast<std::size_t>(back.videoPackets), summary->log.size());
    // 90 pictures at 60 per second: one and a half seconds. The sound follows the clock and may run longer
    // on a slow machine, so the file as a whole is only held to the lower bound.
    EXPECT_NEAR(back.videoSeconds, 1.5, 0.05);
    EXPECT_GT(back.seconds, 1.4);
    EXPECT_TRUE(back.bt709);
    ASSERT_TRUE(back.looked);
    // Red in BT.709 has luma 72 (in BT.601 it would be 88) and a high red difference; the rest is dark.
    // The marker stays on top, so the picture is not upside down.
    EXPECT_NEAR(back.markerY, 72, 8);
    EXPECT_GT(back.markerV, 200);
    EXPECT_LT(back.belowY, 60);
    EXPECT_LT(back.bottomLeftY, 60);
    std::filesystem::remove(file);
}

TEST(LinuxVideoRecorder, KeepsGoingWithoutAnySound) {
    std::string why;
    if (!linuxos::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    const auto file = recTempFile("silent");
    std::filesystem::remove(file);
    linuxos::VideoRecorder recorder;
    const auto opened = recorder.start(file, 640, 360, 30);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 30; ++i) {
        std::vector<std::uint8_t> picture(640 * 360 * 4, static_cast<std::uint8_t>(i * 8));
        recAddWaiting(recorder, picture, media::ShownKind::Real, start + std::chrono::milliseconds(33 * i));
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    const auto summary = recorder.stop();
    ASSERT_TRUE(summary) << summary.error().message;
    EXPECT_FALSE(summary->sound);
    // Silence kept the sound up with the clock, so the file is as long as the recording.
    EXPECT_GT(summary->seconds, 0.7);
    const RecReadBack back = recReadFile(file);
    ASSERT_TRUE(back.error.empty()) << back.error;
    EXPECT_EQ(summary->pictures, 30u);
    EXPECT_EQ(static_cast<std::size_t>(back.videoPackets), summary->pictures);
    EXPECT_GT(back.audioPackets, 0);
    // A second stop does nothing harmful.
    EXPECT_FALSE(recorder.stop());
    std::filesystem::remove(file);
}

TEST(LinuxVideoRecorder, LeavesPicturesOutWhenTheWriterIsBehind) {
    std::string why;
    if (!linuxos::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    const auto file = recTempFile("behind");
    std::filesystem::remove(file);
    linuxos::VideoRecorder recorder;
    ASSERT_TRUE(recorder.start(file, 1280, 720, 60));
    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> picture(1280 * 720 * 4, 90);
    int taken = 0;
    // Forty pictures at once: the queue holds eight, the rest is refused instead of waiting.
    for (int i = 0; i < 40; ++i) {
        if (recorder.addPicture(std::vector<std::uint8_t>(picture), media::ShownKind::Real,
                                start + std::chrono::milliseconds(16 * i))) {
            ++taken;
        }
    }
    EXPECT_FALSE(recorder.addPicture(std::vector<std::uint8_t>(10), media::ShownKind::Real, start)); // wrong size
    const auto summary = recorder.stop();
    ASSERT_TRUE(summary) << summary.error().message;
    std::printf("Behind: %d of 40 taken, %zu left out\n", taken, summary->dropped);
    EXPECT_LT(taken, 40);
    EXPECT_EQ(summary->pictures, static_cast<std::size_t>(taken));
    EXPECT_EQ(summary->dropped, static_cast<std::size_t>(40 - taken));
    // The buffers of left-out pictures come back for reuse while it runs; after stop there are none.
    EXPECT_TRUE(recorder.buffer().empty());
    std::filesystem::remove(file);
}

// Each picture encoder this FFmpeg has, alone: a build without x264 (vcpkg's, say) records with the next one.
TEST(LinuxVideoRecorder, RecordsWithEachPictureEncoderThereIs) {
    std::string why;
    if (!linuxos::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    constexpr int kW = 640;
    constexpr int kH = 360;
    std::vector<std::uint8_t> picture(static_cast<std::size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            std::uint8_t* pixel = &picture[(static_cast<std::size_t>(y) * kW + x) * 4];
            const bool marker = x < kRecMarker && y < kRecMarker;
            pixel[0] = marker ? 20 : 50;
            pixel[1] = marker ? 20 : 50;
            pixel[2] = marker ? 235 : 50;
            pixel[3] = 255;
        }
    }
    int tried = 0;
    for (const char* name : {"libx264", "libopenh264", "mpeg4"}) {
        if (!avcodec_find_encoder_by_name(name)) {
            std::printf("Encoder %s: not in this FFmpeg\n", name);
            continue;
        }
        ++tried;
        const auto file = recTempFile(name);
        std::filesystem::remove(file);
        linuxos::VideoRecorder recorder;
        recorder.useOnlyEncoder(name);
        const auto opened = recorder.start(file, kW, kH, 60);
        ASSERT_TRUE(opened) << name << ": " << opened.error().message;
        EXPECT_EQ(recorder.encoder(), name);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) {
            recAddWaiting(recorder, picture, media::ShownKind::Real, start + std::chrono::microseconds(16'667LL * i));
        }
        const auto summary = recorder.stop();
        ASSERT_TRUE(summary) << name << ": " << summary.error().message;
        const RecReadBack back = recReadFile(file);
        ASSERT_TRUE(back.error.empty()) << name << ": " << back.error;
        std::printf("Encoder %s: %zu pictures, read back %s with %d packets, %.2f s, marker Y %.0f V %.0f\n", name,
                    summary->pictures, back.videoCodec.c_str(), back.videoPackets, back.seconds, back.markerY,
                    back.markerV);
        EXPECT_EQ(summary->pictures, 30u) << name;
        EXPECT_EQ(static_cast<std::size_t>(back.videoPackets), summary->pictures) << name;
        EXPECT_EQ(back.videoCodec, std::string(name) == "mpeg4" ? "mpeg4" : "h264");
        ASSERT_TRUE(back.looked) << name;
        EXPECT_NEAR(back.markerY, 72, 10) << name;
        EXPECT_GT(back.markerV, 190) << name;
        std::filesystem::remove(file);
    }
    EXPECT_GT(tried, 0);
}

TEST(LinuxVideoRecorder, RefusesAnOddSizeAndAFileItCannotCreate) {
    std::string why;
    if (!linuxos::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    linuxos::VideoRecorder recorder;
    EXPECT_FALSE(recorder.start(recTempFile("odd"), 641, 360, 60));
    EXPECT_FALSE(recorder.running());
    const auto failed = recorder.start("/proc/veloplay-cannot-be-here/x.mp4", 640, 360, 60);
    ASSERT_FALSE(failed);
    EXPECT_FALSE(recorder.running());
    EXPECT_NE(failed.error().message.find("creating the file"), std::string::npos) << failed.error().message;
    // After a failed start it can still record.
    const auto file = recTempFile("again");
    ASSERT_TRUE(recorder.start(file, 640, 360, 30));
    EXPECT_TRUE(recorder.stop());
    std::filesystem::remove(file);
}
