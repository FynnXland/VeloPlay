#include "record/VideoRecorder.hpp"

#include <windows.h>

#include <initguid.h> // defines the GUIDs of the headers below in this file, nothing has to link mfuuid

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>

#include <spdlog/spdlog.h>

#include "record/MediaFoundation.hpp"

namespace veloplay::record {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSoundRate = 48'000;
constexpr int kSoundChannels = 2;
// More than this many pictures waiting means the encoder is behind; newer ones are left out.
constexpr std::size_t kMostWaiting = 8;

std::string hresultText(const char* what, HRESULT result) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", what, static_cast<unsigned long>(result));
    return text;
}

HRESULT setSize(IMFMediaType* type, REFGUID key, UINT32 high, UINT32 low) {
    return type->SetUINT64(key, (static_cast<UINT64>(high) << 32) | low);
}

} // namespace

VideoRecorder::~VideoRecorder() { (void)stop(); }

bool VideoRecorder::available(std::string* why) { return MediaFoundation::get(why) != nullptr; }

double VideoRecorder::seconds() const {
    if (!running_.load()) return 0.0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

Result<bool> VideoRecorder::start(const std::filesystem::path& file, int width, int height, int fps) {
    if (running_.load()) return Error{ErrorCode::InvalidArgument, "a recording is already running"};
    std::string why;
    if (!MediaFoundation::get(&why)) return Error{ErrorCode::Io, why};
    if (width < 64 || height < 64 || width % 2 != 0 || height % 2 != 0 || width > 4096 || height > 4096) {
        return Error{ErrorCode::InvalidArgument, "a recording needs an even size between 64 and 4096 pixels"};
    }
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    width_ = width;
    height_ = height;
    fps_ = std::clamp(fps, 24, 240);
    file_ = file;
    {
        std::lock_guard lock(mutex_);
        stopping_ = false;
        pictures_.clear();
        sound_.clear();
        dropped_ = 0;
        soundArrived_ = false;
        error_.clear();
        summary_ = RecordingSummary{};
    }
    started_ = std::chrono::steady_clock::now();

    // The writer is made on its own thread, which owns it from start to finish; whether that worked comes back here.
    std::promise<std::string> opened;
    std::future<std::string> outcome = opened.get_future();
    running_.store(true);
    thread_ = std::thread([this, &opened] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const MediaFoundation& mf = *MediaFoundation::get();
        bool started = false;
        std::string failure;
        ComPtr<IMFSinkWriter> writer;
        DWORD video = 0;
        DWORD audio = 0;
        do {
            HRESULT result = mf.startup(MF_VERSION, MFSTARTUP_LITE);
            if (FAILED(result)) { failure = hresultText("starting Media Foundation", result); break; }
            started = true;
            ComPtr<IMFAttributes> attributes;
            mf.createAttributes(attributes.GetAddressOf(), 3);
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            // The pictures are queued here already; the writer must not hold back on top of that.
            attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            result = mf.createSinkWriterFromUrl(file_.c_str(), nullptr, attributes.Get(), writer.GetAddressOf());
            if (FAILED(result)) { failure = hresultText("creating the file", result); break; }

            // About 0.16 bits per pixel: 20 Mbit/s for 1080p at 60, plenty for a stream of 15.
            const UINT32 bitrate = static_cast<UINT32>(
                std::clamp(static_cast<double>(width_) * height_ * fps_ * 0.16, 4.0e6, 60.0e6));
            ComPtr<IMFMediaType> out;
            mf.createMediaType(out.GetAddressOf());
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
            out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            setSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
            setSize(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
            setSize(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            result = writer->AddStream(out.Get(), &video);
            if (FAILED(result)) { failure = hresultText("adding the picture", result); break; }
            ComPtr<IMFMediaType> in;
            mf.createMediaType(in.GetAddressOf());
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            setSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
            setSize(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
            setSize(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            // Rows from top to bottom. Without this, RGB counts as bottom-up and the video comes out upside down.
            in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width_ * 4));
            result = writer->SetInputMediaType(video, in.Get(), nullptr);
            if (FAILED(result)) { failure = hresultText("setting up the picture's encoder", result); break; }

            ComPtr<IMFMediaType> soundOut;
            mf.createMediaType(soundOut.GetAddressOf());
            soundOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            soundOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            soundOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            soundOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSoundRate);
            soundOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kSoundChannels);
            soundOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24'000); // 192 kbit/s
            result = writer->AddStream(soundOut.Get(), &audio);
            if (FAILED(result)) { failure = hresultText("adding the sound", result); break; }
            ComPtr<IMFMediaType> soundIn;
            mf.createMediaType(soundIn.GetAddressOf());
            soundIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            soundIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            soundIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            soundIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSoundRate);
            soundIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kSoundChannels);
            soundIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kSoundChannels * 2);
            soundIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSoundRate * kSoundChannels * 2);
            result = writer->SetInputMediaType(audio, soundIn.Get(), nullptr);
            if (FAILED(result)) { failure = hresultText("setting up the sound's encoder", result); break; }
            result = writer->BeginWriting();
            if (FAILED(result)) { failure = hresultText("beginning to write", result); break; }
        } while (false);

        opened.set_value(failure);
        if (failure.empty()) {
            write(writer.Get(), video, audio);
        }
        writer.Reset();
        if (started) mf.shutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    });
    const std::string failure = outcome.get();
    if (!failure.empty()) {
        thread_.join();
        running_.store(false);
        std::error_code removeError;
        std::filesystem::remove(file_, removeError);
        return Error{ErrorCode::Io, failure};
    }
    spdlog::info("Recording: {} x {} at {} fps", width_, height_, fps_);
    return true;
}

std::vector<std::uint8_t> VideoRecorder::buffer() {
    std::lock_guard lock(mutex_);
    if (spare_.empty()) return {};
    std::vector<std::uint8_t> reused = std::move(spare_.back());
    spare_.pop_back();
    return reused;
}

bool VideoRecorder::addPicture(std::vector<std::uint8_t>&& bgra, media::ShownKind kind,
                               std::chrono::steady_clock::time_point at) {
    if (!running_.load() || bgra.size() != static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (pictures_.size() >= kMostWaiting) {
            ++dropped_;
            spare_.push_back(std::move(bgra));
            return false;
        }
        pictures_.push_back(Picture{std::move(bgra), kind, at});
    }
    wake_.notify_one();
    return true;
}

void VideoRecorder::addSound(std::span<const std::int16_t> interleaved, int sampleRate, int channels) {
    if (!running_.load() || sampleRate != kSoundRate || channels != kSoundChannels || interleaved.empty()) return;
    std::lock_guard lock(mutex_);
    soundArrived_ = true;
    // More than two seconds waiting means the writer is gone; the sound would only pile up.
    if (sound_.size() > static_cast<std::size_t>(kSoundRate * kSoundChannels * 2)) return;
    sound_.insert(sound_.end(), interleaved.begin(), interleaved.end());
}

void VideoRecorder::write(IMFSinkWriter* writer, unsigned long videoStream, unsigned long audioStream) {
    const MediaFoundation& mf = *MediaFoundation::get();
    std::int64_t soundSamples = 0; // per channel, written so far
    const LONGLONG frameLength = 10'000'000 / fps_;
    std::vector<std::int16_t> sound;
    std::deque<Picture> pictures;
    std::string failure;

    const auto writeSound = [&](const std::int16_t* data, std::size_t frames) {
        while (frames > 0 && failure.empty()) {
            const std::size_t chunk = std::min<std::size_t>(frames, 4'800); // 100 ms
            const DWORD bytes = static_cast<DWORD>(chunk * kSoundChannels * sizeof(std::int16_t));
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            BYTE* into = nullptr;
            if (FAILED(mf.createMemoryBuffer(bytes, buffer.GetAddressOf())) || FAILED(buffer->Lock(&into, nullptr, nullptr))) {
                failure = "no memory for sound";
                return;
            }
            if (data) std::memcpy(into, data, bytes);
            else std::memset(into, 0, bytes);
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
            mf.createSample(sample.GetAddressOf());
            sample->AddBuffer(buffer.Get());
            sample->SetSampleTime(media::sampleTime(soundSamples, kSoundRate));
            sample->SetSampleDuration(media::sampleTime(static_cast<std::int64_t>(chunk), kSoundRate));
            const HRESULT result = writer->WriteSample(audioStream, sample.Get());
            if (FAILED(result)) failure = hresultText("writing sound", result);
            soundSamples += static_cast<std::int64_t>(chunk);
            if (data) data += chunk * kSoundChannels;
            frames -= chunk;
        }
    };

    for (;;) {
        bool stopping = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20), [&] { return stopping_ || !pictures_.empty(); });
            stopping = stopping_;
            pictures.swap(pictures_);
            sound.swap(sound_);
        }
        if (failure.empty() && !sound.empty()) writeSound(sound.data(), sound.size() / kSoundChannels);
        sound.clear();
        // Without sound arriving, silence keeps the sound up with the clock; otherwise the file stalls.
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        if (failure.empty()) {
            if (const std::int64_t silence = media::silenceToAdd(soundSamples, elapsed, kSoundRate); silence > 0) {
                writeSound(nullptr, static_cast<std::size_t>(silence));
            }
        }
        for (Picture& picture : pictures) {
            if (failure.empty()) {
                const DWORD bytes = static_cast<DWORD>(picture.pixels.size());
                ComPtr<IMFMediaBuffer> buffer;
                ComPtr<IMFSample> sample;
                BYTE* into = nullptr;
                if (FAILED(mf.createMemoryBuffer(bytes, buffer.GetAddressOf())) || FAILED(buffer->Lock(&into, nullptr, nullptr))) {
                    failure = "no memory for a picture";
                } else {
                    std::memcpy(into, picture.pixels.data(), bytes);
                    buffer->Unlock();
                    buffer->SetCurrentLength(bytes);
                    mf.createSample(sample.GetAddressOf());
                    sample->AddBuffer(buffer.Get());
                    // Timed by when the picture was on screen: a refresh the window missed leaves a gap, not a drift.
                    const LONGLONG time = std::max<LONGLONG>(
                        0, std::chrono::duration_cast<std::chrono::nanoseconds>(picture.at - started_).count() / 100);
                    sample->SetSampleTime(time);
                    sample->SetSampleDuration(frameLength);
                    const HRESULT result = writer->WriteSample(videoStream, sample.Get());
                    if (FAILED(result)) failure = hresultText("writing a picture", result);
                    else summary_.log.add(picture.kind);
                }
            }
            std::lock_guard lock(mutex_);
            spare_.push_back(std::move(picture.pixels));
        }
        pictures.clear();
        if (stopping) break;
    }
    {
        const HRESULT result = writer->Finalize();
        if (FAILED(result) && failure.empty()) failure = hresultText("closing the file", result);
    }
    std::lock_guard lock(mutex_);
    error_ = failure;
    summary_.seconds = static_cast<double>(soundSamples) / kSoundRate;
}

Result<RecordingSummary> VideoRecorder::stop() {
    if (!thread_.joinable()) return Error{ErrorCode::InvalidArgument, "no recording is running"};
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    thread_.join();
    running_.store(false);
    std::lock_guard lock(mutex_);
    summary_.file = file_;
    summary_.pictures = summary_.log.size();
    summary_.dropped = dropped_;
    summary_.sound = soundArrived_;
    spare_.clear();
    if (!error_.empty()) {
        // A file without a single picture does not play; nothing worth keeping.
        if (summary_.pictures == 0) {
            std::error_code ignored;
            std::filesystem::remove(file_, ignored);
        }
        return Error{ErrorCode::Io, error_};
    }
    spdlog::info("Recording: {} pictures ({} computed, {} repeated), {} left out, {:.1f} s, sound {}", summary_.pictures,
                 summary_.log.count(media::ShownKind::Computed), summary_.log.count(media::ShownKind::Repeated),
                 summary_.dropped, summary_.seconds, summary_.sound ? "yes" : "no");
    return summary_;
}

} // namespace veloplay::record
