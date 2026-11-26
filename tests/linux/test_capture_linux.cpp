// The Linux capture card in parts: listing, the choice of format, colours,
// the conversions and the session's behaviour when a device cannot be opened.
// No real device is opened here: on a developer's machine a webcam would switch on.
#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <linux/videodev2.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "app/CaptureMatch.hpp"
#include "linux/V4l2Capture.hpp"

using namespace veloplay;

namespace {

/// Its own name per process: other checkouts may run the same tests at the same time.
std::filesystem::path capEmptyFolder(const char* name) {
    const auto folder = std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

/// A JPEG of one flat colour, made with FFmpeg's own encoder, in the chroma layout asked for.
std::vector<std::uint8_t> capJpeg(int width, int height, AVPixelFormat layout, std::uint8_t y, std::uint8_t u,
                                  std::uint8_t v) {
    std::vector<std::uint8_t> out;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return out;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    context->width = width;
    context->height = height;
    context->pix_fmt = layout;
    context->time_base = AVRational{1, 30};
    context->color_range = AVCOL_RANGE_JPEG;
    context->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    context->flags |= AV_CODEC_FLAG_QSCALE;
    context->global_quality = FF_QP2LAMBDA * 2;
    if (avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return out;
    }
    AVFrame* frame = av_frame_alloc();
    frame->format = layout;
    frame->width = width;
    frame->height = height;
    frame->color_range = AVCOL_RANGE_JPEG;
    av_frame_get_buffer(frame, 0);
    const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(layout);
    for (int plane = 0; plane < 3; ++plane) {
        const int rows = plane == 0 ? height : AV_CEIL_RSHIFT(height, description->log2_chroma_h);
        const std::uint8_t value = plane == 0 ? y : (plane == 1 ? u : v);
        for (int row = 0; row < rows; ++row) {
            std::memset(frame->data[plane] + static_cast<std::ptrdiff_t>(row) * frame->linesize[plane], value,
                        static_cast<std::size_t>(frame->linesize[plane]));
        }
    }
    frame->pts = 0;
    AVPacket* packet = av_packet_alloc();
    if (avcodec_send_frame(context, frame) >= 0 && avcodec_send_frame(context, nullptr) >= 0 &&
        avcodec_receive_packet(context, packet) >= 0) {
        out.assign(packet->data, packet->data + packet->size);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return out;
}

double capPlaneAverage(const media::VideoFrame& picture, int plane, int width, int height) {
    double sum = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            sum += picture.planes[static_cast<std::size_t>(plane)][static_cast<std::ptrdiff_t>(y) *
                                                                      picture.strides[static_cast<std::size_t>(plane)] + x];
        }
    }
    return sum / (width * height);
}

struct CapNullSink final : session::IMediaSink {
    bool onVideo(std::span<const std::uint8_t>, int, bool) override { return false; }
    void onAudioFormat(const session::AudioFormat&) override {}
    void onAudio(std::span<const std::uint8_t>) override {}
};

} // namespace

TEST(LinuxCapture, ListsNothingWhereThereAreNoVideoDevices) {
    std::string why = "unchanged";
    EXPECT_TRUE(linuxos::listCaptureDevices(&why, capEmptyFolder("veloplay-capture-empty")).empty());
    EXPECT_TRUE(why.empty());
    EXPECT_TRUE(linuxos::listCaptureDevices(nullptr, "/veloplay-no-such-folder").empty());

    // Files that are named like devices but are none: asked, found wanting, left out.
    const auto folder = capEmptyFolder("veloplay-capture-files");
    std::ofstream(folder / "video0") << "not a device";
    std::ofstream(folder / "video12") << "";
    std::ofstream(folder / "videos") << "";
    std::filesystem::create_directories(folder / "video3");
    EXPECT_TRUE(linuxos::listCaptureDevices(&why, folder).empty());
    std::filesystem::remove_all(folder);
}

TEST(LinuxCapture, ChoosesUpTo1080pThenTheMostPicturesThenUncompressed) {
    using linuxos::CaptureMode;
    // What a typical USB capture card offers: YUYV is slow at 1080p, MJPEG gives 60.
    const std::vector<CaptureMode> card = {
        {V4L2_PIX_FMT_YUYV, 1920, 1080, 5},  {V4L2_PIX_FMT_YUYV, 1280, 720, 10},
        {V4L2_PIX_FMT_MJPEG, 1920, 1080, 60}, {V4L2_PIX_FMT_MJPEG, 1280, 720, 60},
        {V4L2_PIX_FMT_MJPEG, 3840, 2160, 30},
    };
    auto chosen = linuxos::chooseCaptureMode(card);
    ASSERT_TRUE(chosen);
    EXPECT_EQ(chosen->format, static_cast<std::uint32_t>(V4L2_PIX_FMT_MJPEG));
    EXPECT_EQ(chosen->width, 1920);
    EXPECT_DOUBLE_EQ(chosen->fps, 60);

    // Same size and rate: uncompressed wins, NV12 first among those.
    chosen = linuxos::chooseCaptureMode({{V4L2_PIX_FMT_MJPEG, 1920, 1080, 60},
                                         {V4L2_PIX_FMT_YUYV, 1920, 1080, 60},
                                         {V4L2_PIX_FMT_NV12, 1920, 1080, 60}});
    ASSERT_TRUE(chosen);
    EXPECT_EQ(chosen->format, static_cast<std::uint32_t>(V4L2_PIX_FMT_NV12));

    // Larger than 1080p only when nothing else is there.
    chosen = linuxos::chooseCaptureMode({{V4L2_PIX_FMT_MJPEG, 3840, 2160, 60}, {V4L2_PIX_FMT_MJPEG, 1920, 1080, 30}});
    ASSERT_TRUE(chosen);
    EXPECT_EQ(chosen->width, 1920);
    chosen = linuxos::chooseCaptureMode({{V4L2_PIX_FMT_MJPEG, 3840, 2160, 60}});
    ASSERT_TRUE(chosen);
    EXPECT_EQ(chosen->width, 3840);

    // Formats it cannot read are never chosen.
    EXPECT_FALSE(linuxos::chooseCaptureMode({{V4L2_PIX_FMT_H264, 1920, 1080, 60}}));
    EXPECT_FALSE(linuxos::chooseCaptureMode({}));
}

TEST(LinuxCapture, TakesTheColoursTheDriverNames) {
    // Said nothing: BT.709 for HD, as HDMI sends it, BT.601 below.
    EXPECT_FALSE(linuxos::captureColor(0, 0, 0, 1080).bt601);
    EXPECT_TRUE(linuxos::captureColor(0, 0, 0, 480).bt601);
    EXPECT_FALSE(linuxos::captureColor(0, 0, 0, 1080).fullRange);
    // REC709 by default means the BT.709 matrix in limited range.
    auto color = linuxos::captureColor(V4L2_COLORSPACE_REC709, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT, 1080);
    EXPECT_FALSE(color.bt601);
    EXPECT_FALSE(color.fullRange);
    // sRGB (what uvcvideo says without a colour descriptor) means BT.601, limited.
    color = linuxos::captureColor(V4L2_COLORSPACE_SRGB, V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT, 1080);
    EXPECT_TRUE(color.bt601);
    EXPECT_FALSE(color.fullRange);
    // JPEG means full range.
    EXPECT_TRUE(linuxos::captureColor(V4L2_COLORSPACE_JPEG, 0, 0, 1080).fullRange);
    // What the driver says outright wins over the defaults.
    color = linuxos::captureColor(V4L2_COLORSPACE_SRGB, V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_FULL_RANGE, 1080);
    EXPECT_FALSE(color.bt601);
    EXPECT_TRUE(color.fullRange);
}

TEST(LinuxCapture, TurnsYuyvIntoNv12) {
    // Four by three pixels, rows padded to 12 bytes. Luma counts up; the colour differs per row.
    constexpr int kW = 4;
    constexpr int kH = 3;
    constexpr int kStride = 12;
    std::vector<std::uint8_t> yuyv(kStride * kH, 0xEE);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; x += 2) {
            std::uint8_t* pair = &yuyv[static_cast<std::size_t>(y * kStride + x * 2)];
            pair[0] = static_cast<std::uint8_t>(10 * y + x);
            pair[1] = static_cast<std::uint8_t>(100 + 10 * y); // U
            pair[2] = static_cast<std::uint8_t>(10 * y + x + 1);
            pair[3] = static_cast<std::uint8_t>(200 - 10 * y); // V
        }
    }
    std::vector<std::uint8_t> luma(kW * kH, 0);
    std::vector<std::uint8_t> chroma(kW * 2, 0);
    linuxos::yuyvToNv12(yuyv.data(), kStride, kW, kH, luma.data(), kW, chroma.data(), kW);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) EXPECT_EQ(luma[static_cast<std::size_t>(y * kW + x)], 10 * y + x);
    }
    // Rows 0 and 1 averaged; row 2 stands alone.
    EXPECT_EQ(chroma[0], 105);
    EXPECT_EQ(chroma[1], 195);
    EXPECT_EQ(chroma[2], 105);
    EXPECT_EQ(chroma[3], 195);
    EXPECT_EQ(chroma[4], 120);
    EXPECT_EQ(chroma[5], 180);
}

TEST(LinuxCapture, DecodesMjpegInTheRangeOfTheJpeg) {
    const std::vector<std::uint8_t> jpeg422 = capJpeg(64, 48, AV_PIX_FMT_YUV422P, 230, 90, 160);
    const std::vector<std::uint8_t> jpeg420 = capJpeg(64, 48, AV_PIX_FMT_YUV420P, 20, 140, 110);
    if (jpeg422.empty() || jpeg420.empty()) GTEST_SKIP() << "this FFmpeg cannot write JPEG";

    linuxos::MjpegDecoder decoder;
    // 4:2:2, as most cards send it: NV12, the values untouched (full range, not squeezed into 16..235).
    auto picture = decoder.decode(jpeg422);
    ASSERT_TRUE(picture) << picture.error().message;
    EXPECT_EQ(picture->layout, media::PixelLayout::Nv12);
    EXPECT_EQ(picture->width, 64);
    EXPECT_EQ(picture->height, 48);
    EXPECT_TRUE(picture->color.fullRange);
    EXPECT_TRUE(picture->color.bt601);
    EXPECT_NEAR(capPlaneAverage(*picture, 0, 64, 48), 230, 2);
    double u = 0, v = 0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 32; ++x) {
            u += picture->planes[1][y * picture->strides[1] + x * 2];
            v += picture->planes[1][y * picture->strides[1] + x * 2 + 1];
        }
    }
    EXPECT_NEAR(u / (24 * 32), 90, 2);
    EXPECT_NEAR(v / (24 * 32), 160, 2);

    // 4:2:0: handed on as it is.
    picture = decoder.decode(jpeg420);
    ASSERT_TRUE(picture) << picture.error().message;
    EXPECT_EQ(picture->layout, media::PixelLayout::I420);
    EXPECT_TRUE(picture->owner);
    EXPECT_NEAR(capPlaneAverage(*picture, 0, 64, 48), 20, 2);
    EXPECT_NEAR(capPlaneAverage(*picture, 1, 32, 24), 140, 2);
    EXPECT_NEAR(capPlaneAverage(*picture, 2, 32, 24), 110, 2);

    // Garbage, as a USB hiccup gives: an error, and the next picture still decodes.
    const std::vector<std::uint8_t> garbage(500, 0x5A);
    EXPECT_FALSE(decoder.decode(garbage));
    EXPECT_FALSE(decoder.decode({}));
    EXPECT_TRUE(decoder.decode(jpeg422));
}

TEST(LinuxCapture, SessionSaysWhyADeviceCannotBeOpened) {
    CapNullSink sink;
    linuxos::CaptureSession session;
    session::SessionConfig config;
    config.host = "/veloplay-no-such-folder/video0";
    config.name = "USB3 Capture";
    ASSERT_TRUE(session.start(config, &sink));
    std::vector<session::SessionEvent> events;
    for (int turn = 0; turn < 200 && events.empty(); ++turn) {
        events = session.pollEvents();
        if (events.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, session::SessionEvent::Type::Quit);
    EXPECT_EQ(events[0].reason, session::QuitReason::SetupFailed);
    EXPECT_NE(events[0].detail.find("unplugged"), std::string::npos) << events[0].detail;
    EXPECT_TRUE(session.pollEvents().empty()); // Quit is the last
    EXPECT_FALSE(session.submitLoginPin("1234"));
    EXPECT_FALSE(session.sleepConsole());
    session.stop();
    session.stop();

    // A file that is no device: opened, asked, refused.
    const auto folder = capEmptyFolder("veloplay-capture-session");
    std::ofstream(folder / "video0") << "not a device";
    linuxos::CaptureSession other;
    config.host = (folder / "video0").string();
    ASSERT_TRUE(other.start(config, &sink));
    events.clear();
    for (int turn = 0; turn < 200 && events.empty(); ++turn) {
        events = other.pollEvents();
        if (events.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].reason, session::QuitReason::SetupFailed);
    EXPECT_NE(events[0].detail.find("no video device"), std::string::npos) << events[0].detail;
    std::filesystem::remove_all(folder);
}

TEST(LinuxCapture, StoppedBeforeItOpenedSaysStopped) {
    CapNullSink sink;
    linuxos::CaptureSession session;
    session::SessionConfig config;
    config.host = "/veloplay-no-such-folder/video0";
    ASSERT_TRUE(session.start(config, &sink));
    session.stop();
    const auto events = session.pollEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, session::SessionEvent::Type::Quit);
    EXPECT_EQ(events[0].reason, session::QuitReason::Stopped);
}

// The names as Linux gives them: V4L2 calls the card "<name>: <name>" (cut at 31 bytes), PipeWire and
// PulseAudio call its sound input "<name> Analog Stereo" or "... Digital Stereo (IEC958)".
TEST(LinuxCapture, FindsTheSoundInputByTheNamesLinuxGives) {
    const std::vector<std::string> sounds = {"Built-in Audio Analog Stereo", "USB3 Capture Analog Stereo",
                                             "DualSense Wireless Controller Multichannel"};
    EXPECT_EQ(pickSoundFor("USB3 Capture: USB3 Capture", sounds), 1);
    const std::vector<std::string> elgato = {"Built-in Audio Analog Stereo", "Game Capture HD60 S+ Digital Stereo (IEC958)"};
    EXPECT_EQ(pickSoundFor("Game Capture HD60 S+: Game Capt", elgato), 1);
    // A webcam must not take the PC's microphone.
    EXPECT_EQ(pickSoundFor("Integrated Camera: Integrated C", sounds), -1);
    // Known gap: a card that calls itself only by common words finds no sound input (shared rule with Windows).
    EXPECT_EQ(pickSoundFor("USB Video: USB Video", {"MacroSilicon USB Video Analog Stereo"}), -1);
}

TEST(LinuxCapture, AFactoryMakesCaptureSessions) {
    auto session = linuxos::captureSessionFactory()();
    ASSERT_TRUE(session);
    EXPECT_NE(dynamic_cast<linuxos::CaptureSession*>(session.get()), nullptr);
}
