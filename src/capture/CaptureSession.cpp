#include "capture/CaptureSession.hpp"

#include <windows.h>

#include <initguid.h> // the GUIDs of the headers below are defined in this file

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <spdlog/spdlog.h>

#include "app/CaptureMatch.hpp"
#include "record/MediaFoundation.hpp"

namespace veloplay::capture {

using Microsoft::WRL::ComPtr;

namespace {

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, size - 1)), '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(0, size - 1)), L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

std::string failed(const char* what, HRESULT result) {
    // Windows' camera privacy setting covers capture cards as well, and that is the usual reason for a no.
    if (result == E_ACCESSDENIED) {
        return std::string(what) + ": Windows denies access to video devices (Settings > Privacy & security > Camera, "
                                   "\"Let desktop apps access your camera\")";
    }
    char text[128];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", what, static_cast<unsigned long>(result));
    return text;
}

struct Found {
    std::string id;
    std::string name;
};

/// Video or sound inputs as Windows lists them. Media Foundation must be started.
std::vector<Found> enumerate(const record::MediaFoundation& mf, bool sound) {
    std::vector<Found> out;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(mf.createAttributes(attributes.GetAddressOf(), 1))) return out;
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, sound ? MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID
                                                                  : MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(mf.enumDeviceSources(attributes.Get(), &devices, &count))) return out;
    const GUID& link = sound ? MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID
                             : MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        WCHAR* id = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) &&
            SUCCEEDED(devices[i]->GetAllocatedString(link, &id, &length))) {
            out.push_back({utf8(id), utf8(name)});
        }
        CoTaskMemFree(name);
        CoTaskMemFree(id);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return out;
}

/// The size to show: a decoder hands out whole blocks (1088 rows for 1080, 368 for 360) and names the
/// part that is picture as the "minimum display aperture". Rows below it are padding, not picture.
void visibleSize(IMFMediaType* type, int& width, int& height) {
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    MFVideoArea area{};
    UINT32 size = 0;
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &size)) &&
        size == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0 && area.Area.cx <= width && area.Area.cy <= height) {
        width = static_cast<int>(area.Area.cx);
        height = static_cast<int>(area.Area.cy);
    }
}

/// Opens one device as a source reader. Low latency: a capture card is watched live, nothing to buffer for.
Result<IMFSourceReader*> openReader(const record::MediaFoundation& mf, const std::string& id, bool sound,
                                    IMFMediaSource** sourceOut) {
    ComPtr<IMFAttributes> attributes;
    mf.createAttributes(attributes.GetAddressOf(), 2);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, sound ? MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID
                                                                  : MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    const std::wstring link = wide(id);
    attributes->SetString(sound ? MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID
                                : MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                          link.c_str());
    ComPtr<IMFMediaSource> source;
    HRESULT result = mf.createDeviceSource(attributes.Get(), source.GetAddressOf());
    if (FAILED(result)) return Error{ErrorCode::Io, failed(sound ? "opening the sound input" : "opening the capture card", result)};
    ComPtr<IMFAttributes> options;
    mf.createAttributes(options.GetAddressOf(), 2);
    options->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    options->SetUINT32(MF_LOW_LATENCY, TRUE);
    IMFSourceReader* reader = nullptr;
    result = mf.createSourceReaderFromMediaSource(source.Get(), options.Get(), &reader);
    if (FAILED(result)) {
        source->Shutdown();
        return Error{ErrorCode::Io, failed("reading from the device", result)};
    }
    *sourceOut = source.Detach();
    return reader;
}

} // namespace

std::vector<CaptureDevice> listDevices(std::string* why) {
    std::vector<CaptureDevice> out;
    const record::MediaFoundation* mf = record::MediaFoundation::get(why);
    if (!mf) return out;
    if (!mf->enumDeviceSources) {
        if (why) *why = "Media Foundation on this PC cannot list video devices";
        return out;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(mf->startup(MF_VERSION, MFSTARTUP_LITE))) {
        for (Found& found : enumerate(*mf, false)) out.push_back({std::move(found.id), std::move(found.name)});
        mf->shutdown();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return out;
}

// ---- CaptureSource ----------------------------------------------------------

CaptureSource::~CaptureSource() { close(); }

void CaptureSource::close() {
    stop();
    if (videoReader_) videoReader_->Release();
    if (soundReader_) soundReader_->Release();
    videoReader_ = nullptr;
    soundReader_ = nullptr;
    if (videoSource_) {
        videoSource_->Shutdown();
        videoSource_->Release();
        videoSource_ = nullptr;
    }
    if (soundSource_) {
        soundSource_->Shutdown();
        soundSource_->Release();
        soundSource_ = nullptr;
    }
    if (started_) record::MediaFoundation::get()->shutdown();
    started_ = false;
}

std::string CaptureSource::failure() {
    std::lock_guard lock(mutex_);
    return failure_;
}

Result<void> CaptureSource::openDevice(const std::string& id, const std::string& name) {
    std::string why;
    const record::MediaFoundation* mf = record::MediaFoundation::get(&why);
    if (!mf) return Error{ErrorCode::Io, why};
    if (!mf->createDeviceSource) return Error{ErrorCode::Io, "Media Foundation on this PC cannot open video devices"};
    if (FAILED(mf->startup(MF_VERSION, MFSTARTUP_LITE))) return Error{ErrorCode::Io, "Media Foundation did not start"};
    started_ = true;

    const auto video = openReader(*mf, id, false, &videoSource_);
    if (!video) return video.error();
    videoReader_ = video.value();
    if (const auto set = setUpVideo(videoReader_, true); !set) return set.error();

    // The card's sound is a device of its own; found by its name. Without it the picture still runs.
    std::vector<Found> inputs = enumerate(*mf, true);
    std::vector<std::string> names;
    for (const Found& input : inputs) names.push_back(input.name);
    const int match = pickSoundFor(name, names);
    if (match < 0) {
        spdlog::info("Capture card: no sound input found that belongs to {}", name);
        return {};
    }
    const auto sound = openReader(*mf, inputs[static_cast<std::size_t>(match)].id, true, &soundSource_);
    if (!sound) {
        spdlog::warn("Capture card: sound input {} could not be opened: {}", names[static_cast<std::size_t>(match)],
                     sound.error().message);
        return {};
    }
    soundReader_ = sound.value();
    if (const auto set = setUpAudio(soundReader_); !set) {
        spdlog::warn("Capture card: sound input {}: {}", names[static_cast<std::size_t>(match)], set.error().message);
        soundReader_->Release();
        soundReader_ = nullptr;
        return {};
    }
    spdlog::info("Capture card: sound from {} ({} Hz, {} channels)", names[static_cast<std::size_t>(match)],
                 soundRate_, soundChannels_);
    return {};
}

Result<void> CaptureSource::openFile(const std::filesystem::path& file) {
    std::string why;
    const record::MediaFoundation* mf = record::MediaFoundation::get(&why);
    if (!mf) return Error{ErrorCode::Io, why};
    if (FAILED(mf->startup(MF_VERSION, MFSTARTUP_LITE))) return Error{ErrorCode::Io, "Media Foundation did not start"};
    started_ = true;
    ComPtr<IMFAttributes> options;
    mf->createAttributes(options.GetAddressOf(), 1);
    options->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT result = mf->createSourceReaderFromUrl(file.c_str(), options.Get(), &videoReader_);
    if (FAILED(result)) return Error{ErrorCode::Io, failed("opening the file", result)};
    if (const auto set = setUpVideo(videoReader_, false); !set) return set.error();
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(videoReader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                                                    native.GetAddressOf()))) {
        fileHasSound_ = static_cast<bool>(setUpAudio(videoReader_));
    }
    return {};
}

Result<void> CaptureSource::setUpVideo(IMFSourceReader* reader, bool pickBest) {
    const record::MediaFoundation& mf = *record::MediaFoundation::get();
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(stream, TRUE);
    if (pickBest) {
        // Cards offer a list of formats. The best for this program: up to 1080p, as many pictures as
        // possible up to 60, uncompressed before compressed (a decoder adds a little delay).
        ComPtr<IMFMediaType> best;
        double bestScore = -1.0;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(reader->GetNativeMediaType(stream, i, type.GetAddressOf()))) break;
            UINT32 width = 0, height = 0, numerator = 0, denominator = 1;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            const double fps = denominator ? static_cast<double>(numerator) / denominator : 0.0;
            const double area = std::min(static_cast<double>(width) * height, 1920.0 * 1080.0);
            const bool compressed = subtype == MFVideoFormat_MJPG || subtype == MFVideoFormat_H264;
            const double score = area * 1000.0 + std::min(fps, 61.0) * 10.0 + (compressed ? 0.0 : 1.0);
            if (score > bestScore) {
                bestScore = score;
                best = type;
            }
        }
        if (best) reader->SetCurrentMediaType(stream, nullptr, best.Get());
    }
    ComPtr<IMFMediaType> wanted;
    mf.createMediaType(wanted.GetAddressOf());
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    const HRESULT result = reader->SetCurrentMediaType(stream, nullptr, wanted.Get());
    if (FAILED(result)) return Error{ErrorCode::Io, failed("asking for the picture in NV12", result)};
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(stream, current.GetAddressOf());
    visibleSize(current.Get(), width_, height_);
    UINT32 stride = 0;
    stride_ = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? std::abs(static_cast<INT32>(stride))
                                                                          : width_;
    if (stride_ < width_) stride_ = width_;
    color_.bt601 = MFGetAttributeUINT32(current.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709) ==
                   MFVideoTransferMatrix_BT601;
    color_.fullRange = MFGetAttributeUINT32(current.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) ==
                       MFNominalRange_0_255;
    if (width_ < 2 || height_ < 2) return Error{ErrorCode::Io, "the picture has no size"};
    spdlog::info("Capture: picture {} x {}, rows of {} bytes, {}{}", width_, height_, stride_,
                 color_.bt601 ? "BT.601" : "BT.709", color_.fullRange ? ", full range" : "");
    return {};
}

Result<void> CaptureSource::setUpAudio(IMFSourceReader* reader) {
    const record::MediaFoundation& mf = *record::MediaFoundation::get();
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(stream, TRUE);
    // 48 kHz stereo is what the sound output and a recording like best; the reader converts if it can.
    ComPtr<IMFMediaType> wanted;
    mf.createMediaType(wanted.GetAddressOf());
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    wanted->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000);
    wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    wanted->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    wanted->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192'000);
    HRESULT result = reader->SetCurrentMediaType(stream, nullptr, wanted.Get());
    if (FAILED(result)) {
        // Then in its own rate and channels, as long as it is 16-bit samples.
        ComPtr<IMFMediaType> partial;
        mf.createMediaType(partial.GetAddressOf());
        partial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        partial->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        partial->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        result = reader->SetCurrentMediaType(stream, nullptr, partial.Get());
    }
    if (FAILED(result)) {
        reader->SetStreamSelection(stream, FALSE);
        return Error{ErrorCode::Io, failed("asking for the sound as 16-bit samples", result)};
    }
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(stream, current.GetAddressOf());
    soundRate_ = static_cast<int>(MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
    soundChannels_ = static_cast<int>(MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
    if (soundRate_ <= 0 || soundChannels_ <= 0) {
        reader->SetStreamSelection(stream, FALSE);
        return Error{ErrorCode::Io, "the sound has no format"};
    }
    return {};
}

void CaptureSource::start(session::IMediaSink* sink) {
    sink_.store(sink);
    stopping_.store(false);
    ended_.store(false);
    if (sink && soundRate_ > 0 && (soundReader_ || fileHasSound_)) sink->onPcmFormat(soundRate_, soundChannels_);
    if (videoReader_) videoThread_ = std::thread([this] { run(videoReader_, true, fileHasSound_); });
    if (soundReader_) soundThread_ = std::thread([this] { run(soundReader_, false, true); });
}

void CaptureSource::stop() {
    stopping_.store(true);
    // A device without a signal can keep ReadSample waiting; shutting its source down ends the wait.
    if (videoSource_) videoSource_->Shutdown();
    if (soundSource_) soundSource_->Shutdown();
    if (videoThread_.joinable()) videoThread_.join();
    if (soundThread_.joinable()) soundThread_.join();
}

void CaptureSource::run(IMFSourceReader* reader, bool video, bool audio) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool videoOpen = video;
    bool audioOpen = audio;
    while (!stopping_.load() && (videoOpen || audioOpen)) {
        DWORD stream = 0;
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        const HRESULT result = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags,
                                                  &time, sample.GetAddressOf());
        if (FAILED(result)) {
            if (!stopping_.load()) {
                std::lock_guard lock(mutex_);
                failure_ = failed("reading from the capture card", result);
            }
            break;
        }
        // A read that comes back after the end was asked for hands nothing on any more.
        if (stopping_.load()) break;
        session::IMediaSink* const sink = sink_.load();
        ComPtr<IMFMediaType> type;
        GUID major{};
        if (SUCCEEDED(reader->GetCurrentMediaType(stream, type.GetAddressOf()))) type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        const bool isVideo = major == MFMediaType_Video;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            (isVideo ? videoOpen : audioOpen) = false;
            reader->SetStreamSelection(stream, FALSE);
            continue;
        }
        if (flags & MF_SOURCE_READERF_ERROR) {
            std::lock_guard lock(mutex_);
            failure_ = "the capture card reported an error";
            break;
        }
        if (isVideo && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && type) {
            // The card switched formats (another resolution on its input): take the new size.
            UINT32 stride = 0;
            visibleSize(type.Get(), width_, height_);
            stride_ = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? std::abs(static_cast<INT32>(stride))
                                                                               : width_;
            if (stride_ < width_) stride_ = width_;
        }
        if (!sample || !sink) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
        if (isVideo) {
            const std::size_t stride = static_cast<std::size_t>(stride_);
            const std::size_t needed = stride * static_cast<std::size_t>(height_) * 3 / 2;
            if (width_ > 0 && height_ > 0 && length >= needed) {
                // Rows below the picture (1088 for 1080) come before the colour plane: the buffer's own height
                // says where that plane starts. The picture shown is only the visible part.
                std::size_t rows = static_cast<std::size_t>(height_);
                const std::size_t padded = static_cast<std::size_t>(length) * 2 / (stride * 3);
                if (padded > rows && padded <= rows + 32) rows = padded;
                auto pixels = std::make_shared<std::vector<std::uint8_t>>(data, data + stride * rows * 3 / 2);
                media::VideoFrame picture;
                picture.width = width_;
                picture.height = height_;
                picture.layout = media::PixelLayout::Nv12;
                picture.planes = {pixels->data(), pixels->data() + stride * rows, nullptr};
                picture.strides = {static_cast<int>(stride), static_cast<int>(stride), 0};
                picture.color = color_;
                picture.owner = pixels;
                if (sink->onPicture(std::move(picture))) pictures_.fetch_add(1);
            }
        } else if (major == MFMediaType_Audio) {
            sink->onPcm(std::span<const std::int16_t>(reinterpret_cast<const std::int16_t*>(data), length / 2));
        }
        buffer->Unlock();
    }
    if (video) ended_.store(true);
    if (SUCCEEDED(com)) CoUninitialize();
}

// ---- CaptureSession ---------------------------------------------------------

CaptureSession::~CaptureSession() {
    stop();
    // Waits for the reading to end. The program destroys a capture session on a thread of its own (slowToEnd),
    // so a device that never returns from its read holds up that thread and nothing else.
    if (stopper_.joinable()) stopper_.join();
}

Result<void> CaptureSession::start(const session::SessionConfig& config, session::IMediaSink* sink) {
    if (opener_.joinable()) return Error{ErrorCode::InvalidArgument, "already started"};
    std::string why;
    if (!record::MediaFoundation::get(&why)) return Error{ErrorCode::Io, why};
    countedAt_ = session::Clock::now();
    opener_ = std::thread([this, id = config.host, name = config.name, sink] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto opened = source_.openDevice(id, name);
        {
            std::lock_guard lock(mutex_);
            if (!opened) openFailure_ = opened.error().message;
            opened_ = true;
        }
        if (opened && !stopped_) source_.start(sink);
        if (SUCCEEDED(com)) CoUninitialize();
    });
    return {};
}

void CaptureSession::stop() {
    // Called from the window's thread, by the click on "End". It must not wait there: with Windows' virtual
    // camera the read did not come back after its source was shut down, the window waited for the reading
    // thread and Windows closed the program as not responding (1.17.0). The end is reported at once
    // (pollEvents), the waiting happens here on a thread of its own.
    if (stopped_.exchange(true)) return;
    source_.detachSink();
    stopper_ = std::thread([this] {
        // Everything of Media Foundation is given back here, on a thread that has COM, not wherever the
        // session happens to be destroyed.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (opener_.joinable()) opener_.join();
        source_.detachSink(); // the opener may have started the reading in the meantime
        source_.close();
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

std::vector<session::SessionEvent> CaptureSession::pollEvents() {
    std::vector<session::SessionEvent> events;
    if (quitSent_) return events;
    std::string failure;
    bool opened = false;
    {
        std::lock_guard lock(mutex_);
        failure = openFailure_;
        opened = opened_;
    }
    if (!opened && !stopped_) return events;
    if (failure.empty() && source_.ended()) failure = source_.failure().empty() ? "the capture card stopped delivering" : source_.failure();
    if (!announced_ && failure.empty() && !stopped_) {
        // Shown as soon as it is open: a card without a signal on its input still counts as there.
        announced_ = true;
        events.push_back(session::SessionEvent{});
    }
    if (stopped_ || !failure.empty()) {
        session::SessionEvent quit;
        quit.type = session::SessionEvent::Type::Quit;
        quit.reason = stopped_ ? session::QuitReason::Stopped
                               : (announced_ ? session::QuitReason::ConnectionLost : session::QuitReason::SetupFailed);
        quit.detail = stopped_ ? "stopped" : failure;
        quitSent_ = true;
        events.push_back(std::move(quit));
    }
    return events;
}

session::SessionStats CaptureSession::stats(session::Clock::time_point now) {
    const std::uint64_t pictures = source_.pictures();
    const double seconds = std::chrono::duration<double>(now - countedAt_).count();
    if (seconds >= 1.0) {
        fps_ = static_cast<double>(pictures - countedPictures_) / seconds;
        countedPictures_ = pictures;
        countedAt_ = now;
    }
    session::SessionStats out;
    out.streamFps = fps_;
    out.videoFrames = pictures;
    return out;
}

session::SessionFactory captureSessionFactory() {
    return [] { return std::unique_ptr<session::ISession>(std::make_unique<CaptureSession>()); };
}

} // namespace veloplay::capture
