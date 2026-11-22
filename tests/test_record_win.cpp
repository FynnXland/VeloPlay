// The recorder against the real Media Foundation of this machine: write a
// file, read it back and look into the picture.
#include <gtest/gtest.h>

#include <windows.h>

#include <initguid.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include "record/MediaFoundation.hpp"
#include "record/VideoRecorder.hpp"

using Microsoft::WRL::ComPtr;
using namespace veloplay;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kMarker = 160; // a red square in the top left corner, the rest dark grey

std::vector<std::uint8_t> markedPicture(int shade) {
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* pixel = &bgra[(static_cast<std::size_t>(y) * kWidth + x) * 4];
            const bool marker = x < kMarker && y < kMarker;
            pixel[0] = marker ? 20 : static_cast<std::uint8_t>(shade);
            pixel[1] = marker ? 20 : static_cast<std::uint8_t>(shade);
            pixel[2] = marker ? 235 : static_cast<std::uint8_t>(shade);
            pixel[3] = 255;
        }
    }
    return bgra;
}

struct ReadBack {
    int pictures = 0;
    int soundSamples = 0; // blocks of sound, not samples
    bool sawMarker = false;
    double markerRed = 0, markerBlue = 0, belowMarker = 0, bottomLeft = 0;
    std::string error;
};

double average(const BYTE* scan0, LONG pitch, int x0, int y0, int size, int channel) {
    double sum = 0;
    for (int y = y0; y < y0 + size; ++y) {
        for (int x = x0; x < x0 + size; ++x) sum += scan0[static_cast<std::ptrdiff_t>(y) * pitch + x * 4 + channel];
    }
    return sum / (size * size);
}

ReadBack readFile(const std::filesystem::path& file, bool lookInside) {
    ReadBack back;
    const auto& mf = *record::MediaFoundation::get();
    mf.startup(MF_VERSION, MFSTARTUP_LITE);
    {
        ComPtr<IMFAttributes> attributes;
        mf.createAttributes(attributes.GetAddressOf(), 1);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        HRESULT result = mf.createSourceReaderFromUrl(file.c_str(), attributes.Get(), reader.GetAddressOf());
        if (FAILED(result)) {
            back.error = "cannot open the file";
            mf.shutdown();
            return back;
        }
        ComPtr<IMFMediaType> rgb;
        mf.createMediaType(rgb.GetAddressOf());
        rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        result = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgb.Get());
        if (FAILED(result)) {
            back.error = "cannot read the picture as RGB";
            mf.shutdown();
            return back;
        }
        ComPtr<IMFMediaType> current;
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), current.GetAddressOf());
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
        bool ended[2] = {false, false};
        // Bounded, so a reader that never says "end" cannot hang the test run.
        for (int turn = 0; turn < 20'000; ++turn) {
            DWORD stream = 0, flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags, &time,
                                        sample.GetAddressOf());
            if (FAILED(result)) {
                back.error = "reading failed";
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // One stream ended; the other may have more.
                if (stream < 2) ended[stream] = true;
                reader->SetStreamSelection(stream, FALSE);
                if ((ended[0] && ended[1]) || stream >= 2) break;
                continue;
            }
            if (!sample) continue;
            ComPtr<IMFMediaType> type;
            reader->GetCurrentMediaType(stream, type.GetAddressOf());
            GUID major{};
            type->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (major == MFMediaType_Audio) {
                ++back.soundSamples;
                continue;
            }
            ++back.pictures;
            if (back.pictures != 1 || !lookInside) continue;
            // The first row on top and the step to the next, whichever way the rows lie in memory:
            // from IMF2DBuffer where the buffer has it, otherwise from the stride the type names.
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMF2DBuffer> flat;
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            BYTE* plain = nullptr;
            const char* how = "";
            if (SUCCEEDED(sample->GetBufferByIndex(0, buffer.GetAddressOf())) && SUCCEEDED(buffer.As(&flat)) &&
                SUCCEEDED(flat->Lock2D(&scan0, &pitch))) {
                how = "2D buffer";
            } else {
                flat.Reset();
                buffer.Reset();
                UINT32 stride = 0;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf())) &&
                    SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) &&
                    SUCCEEDED(buffer->Lock(&plain, nullptr, nullptr))) {
                    pitch = static_cast<LONG>(static_cast<INT32>(stride));
                    // A negative stride means the rows lie bottom up; the top row is then the last in memory.
                    scan0 = pitch < 0 ? plain + static_cast<std::ptrdiff_t>(-pitch) * (height - 1) : plain;
                    how = "stride of the type";
                }
            }
            if (scan0) {
                std::printf("Looked into the picture through the %s, %u x %u, step %ld\n", how, width, height,
                            static_cast<long>(pitch));
                back.sawMarker = width >= static_cast<UINT32>(kWidth) && height >= static_cast<UINT32>(kHeight);
                if (back.sawMarker) {
                    back.markerRed = average(scan0, pitch, 40, 40, 60, 2);
                    back.markerBlue = average(scan0, pitch, 40, 40, 60, 0);
                    back.belowMarker = average(scan0, pitch, 40, 400, 60, 2);
                    back.bottomLeft = average(scan0, pitch, 40, kHeight - 120, 60, 2);
                }
                if (flat) flat->Unlock2D();
                else buffer->Unlock();
            } else {
                back.error = "cannot look into the picture";
            }
        }
    }
    mf.shutdown();
    return back;
}

} // namespace

TEST(VideoRecorder, WritesAFileThatPlaysWithItsPicturesInOrder) {
    std::string why;
    if (!record::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    const auto file = std::filesystem::temp_directory_path() / "veloplay-recording-test.mp4";
    std::filesystem::remove(file);

    record::VideoRecorder recorder;
    const auto opened = recorder.start(file, kWidth, kHeight, 60);
    // Some servers have Media Foundation but no encoder for H.264; that is the machine, not VeloPlay.
    if (!opened) GTEST_SKIP() << "no recording on this machine: " << opened.error().message;
    const auto start = std::chrono::steady_clock::now();
    const auto began = std::chrono::steady_clock::now();
    constexpr int kPictures = 90;
    for (int i = 0; i < kPictures; ++i) {
        const auto kind = i % 2 == 0 ? media::ShownKind::Real : media::ShownKind::Computed;
        // Waits when the encoder is behind, so every picture goes in and the numbers below are exact.
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (recorder.addPicture(markedPicture(40 + i % 20), kind, start + std::chrono::microseconds(16'667LL * i))) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Sound for this picture: a quiet tone, 800 samples per channel at 48 kHz.
        std::vector<std::int16_t> tone(1600);
        for (int s = 0; s < 800; ++s) {
            const auto v = static_cast<std::int16_t>(3000 * std::sin((i * 800 + s) * 2 * 3.14159265 * 440 / 48000));
            tone[s * 2] = v;
            tone[s * 2 + 1] = v;
        }
        recorder.addSound(tone, 48'000, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    const auto summary = recorder.stop();
    ASSERT_TRUE(summary) << summary.error().message;
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf("Recording 1080p: %zu pictures written, %zu left out, %.2f s for %d pictures\n", summary->pictures,
                summary->dropped, took, kPictures);
    EXPECT_EQ(summary->pictures, static_cast<std::size_t>(kPictures));
    EXPECT_EQ(summary->pictures, summary->log.size());
    EXPECT_EQ(summary->log.count(media::ShownKind::Computed), static_cast<std::size_t>(kPictures / 2));
    EXPECT_TRUE(summary->sound);

    const ReadBack back = readFile(file, true);
    ASSERT_TRUE(back.error.empty()) << back.error;
    std::printf("Read back: %d pictures, %d blocks of sound, marker red %.0f blue %.0f, below %.0f, bottom left %.0f\n",
                back.pictures, back.soundSamples, back.markerRed, back.markerBlue, back.belowMarker, back.bottomLeft);
    // The frame log describes the file: one letter per picture in it.
    EXPECT_EQ(static_cast<std::size_t>(back.pictures), summary->log.size());
    EXPECT_GT(back.soundSamples, 0);
    ASSERT_TRUE(back.sawMarker);
    // Red stays red (no swapped channels) and stays on top (not upside down).
    EXPECT_GT(back.markerRed, 170);
    EXPECT_LT(back.markerBlue, 90);
    EXPECT_LT(back.belowMarker, 90);
    EXPECT_LT(back.bottomLeft, 90);
    std::filesystem::remove(file);
}

TEST(VideoRecorder, KeepsGoingWithoutAnySound) {
    std::string why;
    if (!record::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    const auto file = std::filesystem::temp_directory_path() / "veloplay-recording-silent.mp4";
    std::filesystem::remove(file);
    record::VideoRecorder recorder;
    const auto opened = recorder.start(file, 640, 360, 30);
    if (!opened) GTEST_SKIP() << "no recording on this machine: " << opened.error().message;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 30; ++i) {
        std::vector<std::uint8_t> picture(640 * 360 * 4, static_cast<std::uint8_t>(i * 8));
        for (int attempt = 0; attempt < 2000; ++attempt) {
            if (recorder.addPicture(std::vector<std::uint8_t>(picture), media::ShownKind::Real,
                                    start + std::chrono::milliseconds(33 * i))) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    const auto summary = recorder.stop();
    ASSERT_TRUE(summary) << summary.error().message;
    EXPECT_FALSE(summary->sound);
    // Silence kept the sound up with the clock, so the file is as long as the recording.
    EXPECT_GT(summary->seconds, 0.7);
    const ReadBack back = readFile(file, false);
    ASSERT_TRUE(back.error.empty()) << back.error;
    EXPECT_EQ(summary->pictures, 30u);
    EXPECT_EQ(static_cast<std::size_t>(back.pictures), summary->pictures);
    EXPECT_GT(back.soundSamples, 0);
    std::filesystem::remove(file);
}

TEST(VideoRecorder, RefusesAnOddSize) {
    std::string why;
    if (!record::VideoRecorder::available(&why)) GTEST_SKIP() << why;
    record::VideoRecorder recorder;
    EXPECT_FALSE(recorder.start(std::filesystem::temp_directory_path() / "veloplay-odd.mp4", 641, 360, 60));
    EXPECT_FALSE(recorder.running());
}
