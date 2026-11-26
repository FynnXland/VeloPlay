#include "linux/FfmpegRecorder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <future>

#include <spdlog/spdlog.h>

namespace veloplay::linuxos {

namespace {

constexpr int kSoundRate = 48'000;
constexpr int kSoundChannels = 2;
constexpr std::int64_t kSoundBitrate = 192'000;
// More than this many pictures waiting means the encoder is behind; newer ones are left out.
constexpr std::size_t kMostWaiting = 8;
// Picture times in 1/60000 s: fine enough for screens up to 240 Hz, and the
// MPEG-4 Part 2 encoder refuses a time base with a denominator above 65535.
constexpr AVRational kPictureTimeBase{1, 60'000};
// The writer and the encoder's own threads give way to the stream: a recording
// that falls behind leaves pictures out, a stream that falls behind stutters.
constexpr int kWriterNice = 5;

// In order of preference. H.264 plays everywhere; MPEG-4 Part 2 is built into
// every FFmpeg and still plays in common players, so a recording works even
// with an FFmpeg built without an H.264 encoder.
constexpr std::array<const char*, 3> kPictureEncoders = {"libx264", "libopenh264", "mpeg4"};

std::string avText(const char* what, int code) {
    char reason[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, reason, sizeof(reason));
    return std::string(what) + " failed (" + reason + ")";
}

const AVCodec* firstPictureEncoder() {
    for (const char* name : kPictureEncoders) {
        if (const AVCodec* codec = avcodec_find_encoder_by_name(name)) return codec;
    }
    return nullptr;
}

} // namespace

struct VideoRecorder::Writer {
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    SwsContext* scale = nullptr;
    AVFrame* picture = nullptr;
    AVFrame* sound = nullptr;
    AVPacket* packet = nullptr;
    bool headerWritten = false;
    std::int64_t lastPts = -1;
    std::int64_t soundSamples = 0;   // per channel, handed to the encoder so far
    std::vector<std::int16_t> pending; // interleaved, less than one encoder frame
    std::string encoder;
    std::string only; // set: no other picture encoder is tried

    ~Writer() {
        av_packet_free(&packet);
        av_frame_free(&picture);
        av_frame_free(&sound);
        sws_freeContext(scale);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (format) {
            if (format->pb) avio_closep(&format->pb);
            avformat_free_context(format);
        }
    }

    /// Opens one of the picture encoders, the first that works with these settings.
    std::string openPicture(int width, int height, int fps) {
        // About 0.16 bits per pixel: 20 Mbit/s for 1080p at 60, plenty for a stream of 15.
        const auto bitrate = static_cast<std::int64_t>(
            std::clamp(static_cast<double>(width) * height * fps * 0.16, 4.0e6, 60.0e6));
        std::string failures;
        for (const char* name : kPictureEncoders) {
            if (!only.empty() && only != name) continue;
            const AVCodec* codec = avcodec_find_encoder_by_name(name);
            if (!codec) continue;
            AVCodecContext* context = avcodec_alloc_context3(codec);
            if (!context) return "no memory for the picture's encoder";
            context->width = width;
            context->height = height;
            context->pix_fmt = AV_PIX_FMT_YUV420P;
            context->time_base = kPictureTimeBase;
            context->framerate = AVRational{fps, 1};
            context->sample_aspect_ratio = AVRational{1, 1};
            context->bit_rate = bitrate;
            context->rc_max_rate = bitrate * 3 / 2;
            context->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(bitrate * 2, INT32_MAX));
            context->gop_size = fps * 2; // a key frame every two seconds, so an editor can cut anywhere near
            // What the pictures are, so players do not take HD for SD colours.
            context->color_range = AVCOL_RANGE_MPEG;
            context->colorspace = AVCOL_SPC_BT709;
            context->color_primaries = AVCOL_PRI_BT709;
            context->color_trc = AVCOL_TRC_BT709;
            if (format->oformat->flags & AVFMT_GLOBALHEADER) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* options = nullptr;
            if (std::strcmp(name, "libx264") == 0) {
                // Fast enough for 1080p at 60 next to a running stream on a few cores.
                av_dict_set(&options, "preset", "veryfast", 0);
            } else if (std::strcmp(name, "libopenh264") == 0) {
                av_dict_set(&options, "rc_mode", "bitrate", 0);
                av_dict_set(&options, "allow_skip_frames", "0", 0); // every picture in the file, as the frame log says
            } else {
                context->qmax = 31;
                context->max_b_frames = 0;
            }
            const int opened = avcodec_open2(context, codec, &options);
            av_dict_free(&options);
            if (opened >= 0) {
                video = context;
                encoder = name;
                return {};
            }
            failures += (failures.empty() ? "" : "; ") + avText(name, opened);
            avcodec_free_context(&context);
        }
        return failures.empty() ? "this FFmpeg has no H.264 or MPEG-4 encoder" : failures;
    }

    std::string openSound() {
        const AVCodec* codec = avcodec_find_encoder_by_name("aac");
        if (!codec) return "this FFmpeg has no AAC encoder";
        audio = avcodec_alloc_context3(codec);
        if (!audio) return "no memory for the sound's encoder";
        audio->sample_fmt = AV_SAMPLE_FMT_FLTP; // the only one FFmpeg's own AAC encoder takes
        audio->sample_rate = kSoundRate;
        av_channel_layout_default(&audio->ch_layout, kSoundChannels);
        audio->bit_rate = kSoundBitrate;
        audio->time_base = AVRational{1, kSoundRate};
        if (format->oformat->flags & AVFMT_GLOBALHEADER) audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int opened = avcodec_open2(audio, codec, nullptr);
        if (opened < 0) return avText("opening the AAC encoder", opened);
        return {};
    }

    std::string open(const std::filesystem::path& file, int width, int height, int fps) {
        int result = avformat_alloc_output_context2(&format, nullptr, "mp4", nullptr);
        if (result < 0 || !format) return avText("preparing an MP4 file", result);
        // The encoders come first: the streams take their settings, and the picture's encoder is only known once one opens.
        if (std::string failure = openPicture(width, height, fps); !failure.empty()) return failure;
        if (std::string failure = openSound(); !failure.empty()) return failure;

        videoStream = avformat_new_stream(format, nullptr);
        audioStream = avformat_new_stream(format, nullptr);
        if (!videoStream || !audioStream) return "no memory for the file's streams";
        videoStream->time_base = video->time_base;
        audioStream->time_base = audio->time_base;
        avcodec_parameters_from_context(videoStream->codecpar, video);
        avcodec_parameters_from_context(audioStream->codecpar, audio);

        scale = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                               nullptr, nullptr, nullptr);
        if (!scale) return "no conversion from BGRA to YUV 4:2:0";
        // swscale writes BT.601 unless told otherwise; the stream above says BT.709, limited range.
        sws_setColorspaceDetails(scale, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709), 0,
                                 0, 1 << 16, 1 << 16);

        picture = av_frame_alloc();
        sound = av_frame_alloc();
        packet = av_packet_alloc();
        if (!picture || !sound || !packet) return "no memory for pictures and sound";
        picture->format = AV_PIX_FMT_YUV420P;
        picture->width = width;
        picture->height = height;
        if ((result = av_frame_get_buffer(picture, 0)) < 0) return avText("making room for a picture", result);
        sound->format = AV_SAMPLE_FMT_FLTP;
        sound->sample_rate = kSoundRate;
        av_channel_layout_copy(&sound->ch_layout, &audio->ch_layout);
        sound->nb_samples = audio->frame_size > 0 ? audio->frame_size : 1024;
        if ((result = av_frame_get_buffer(sound, 0)) < 0) return avText("making room for sound", result);

        result = avio_open(&format->pb, file.c_str(), AVIO_FLAG_WRITE);
        if (result < 0) return avText("creating the file", result);
        result = avformat_write_header(format, nullptr);
        if (result < 0) return avText("beginning to write", result);
        headerWritten = true;
        return {};
    }

    /// Hands what the encoder has ready to the file.
    std::string drain(AVCodecContext* context, AVStream* stream) {
        for (;;) {
            const int received = avcodec_receive_packet(context, packet);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return {};
            if (received < 0) return avText("encoding", received);
            av_packet_rescale_ts(packet, context->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int written = av_interleaved_write_frame(format, packet);
            if (written < 0) return avText("writing to the file", written);
        }
    }

    std::string encodePicture(const std::vector<std::uint8_t>& bgra, std::int64_t pts) {
        int result = av_frame_make_writable(picture); // the encoder may still hold the last one
        if (result < 0) return avText("making room for a picture", result);
        const std::uint8_t* source[1] = {bgra.data()};
        const int sourceStride[1] = {video->width * 4};
        sws_scale(scale, source, sourceStride, 0, video->height, picture->data, picture->linesize);
        // Strictly increasing, or the encoder refuses the picture: two pictures on the same tick go one apart.
        picture->pts = std::max(pts, lastPts + 1);
        lastPts = picture->pts;
        result = avcodec_send_frame(video, picture);
        if (result < 0) return avText("encoding a picture", result);
        return drain(video, videoStream);
    }

    /// Encodes `pending` in whole encoder frames; with `all`, the rest as a last, shorter one.
    std::string encodeSound(bool all) {
        const std::size_t frame = static_cast<std::size_t>(sound->nb_samples);
        std::size_t used = 0;
        const bool smallLast = (audio->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
        while (true) {
            const std::size_t left = (pending.size() - used) / kSoundChannels;
            if (left == 0 || (left < frame && !all)) break;
            const std::size_t take = std::min(left, frame);
            int result = av_frame_make_writable(sound);
            if (result < 0) return avText("making room for sound", result);
            auto* leftChannel = reinterpret_cast<float*>(sound->data[0]);
            auto* rightChannel = reinterpret_cast<float*>(sound->data[1]);
            const std::int16_t* from = pending.data() + used;
            for (std::size_t i = 0; i < frame; ++i) {
                // Past the end of the last piece: silence, for an encoder that wants whole frames.
                leftChannel[i] = i < take ? from[i * 2] / 32768.0f : 0.0f;
                rightChannel[i] = i < take ? from[i * 2 + 1] / 32768.0f : 0.0f;
            }
            sound->nb_samples = static_cast<int>(take < frame && smallLast ? take : frame);
            sound->pts = soundSamples;
            result = avcodec_send_frame(audio, sound);
            sound->nb_samples = static_cast<int>(frame);
            if (result < 0) return avText("encoding sound", result);
            soundSamples += static_cast<std::int64_t>(take);
            used += take * kSoundChannels;
            if (std::string failure = drain(audio, audioStream); !failure.empty()) return failure;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(used));
        return {};
    }

    /// Empties both encoders and closes the file so it plays.
    std::string finish(std::string failure) {
        if (!headerWritten) return failure;
        if (failure.empty()) failure = encodeSound(true);
        if (failure.empty() && avcodec_send_frame(video, nullptr) >= 0) failure = drain(video, videoStream);
        if (failure.empty() && avcodec_send_frame(audio, nullptr) >= 0) failure = drain(audio, audioStream);
        const int closed = av_write_trailer(format);
        if (closed < 0 && failure.empty()) failure = avText("closing the file", closed);
        return failure;
    }
};

VideoRecorder::~VideoRecorder() { (void)stop(); }

bool VideoRecorder::available(std::string* why) {
    std::string reason;
    const AVCodec* picture = firstPictureEncoder();
    if (!av_guess_format("mp4", nullptr, nullptr)) reason = "this FFmpeg cannot write MP4 files";
    else if (!avcodec_find_encoder_by_name("aac")) reason = "this FFmpeg has no AAC encoder";
    else if (!picture) reason = "this FFmpeg has no H.264 or MPEG-4 encoder";
    if (why) *why = reason;
    return reason.empty();
}

std::string VideoRecorder::firstEncoder() {
    const AVCodec* picture = firstPictureEncoder();
    return picture ? picture->name : "";
}

double VideoRecorder::seconds() const {
    if (!running_.load()) return 0.0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

std::string VideoRecorder::encoder() const {
    std::lock_guard lock(mutex_);
    return encoder_;
}

Result<bool> VideoRecorder::start(const std::filesystem::path& file, int width, int height, int fps) {
    if (running_.load() || thread_.joinable()) return Error{ErrorCode::InvalidArgument, "a recording is already running"};
    std::string why;
    if (!available(&why)) return Error{ErrorCode::Io, why};
    // x264 talks on stderr at every start and stop; what matters comes back through the results here.
    if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);
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
        encoder_.clear();
        error_.clear();
        summary_ = RecordingSummary{};
    }
    started_ = std::chrono::steady_clock::now();

    // FFmpeg's state is made on the writing thread, which owns it from start to finish; whether that worked comes back here.
    std::promise<std::string> opened;
    std::future<std::string> outcome = opened.get_future();
    running_.store(true);
    thread_ = std::thread([this, &opened] {
        // Linux keeps the nice value per thread, and threads made from here (x264's) take it over.
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), kWriterNice);
        Writer writer;
        writer.only = onlyEncoder_;
        std::string failure = writer.open(file_, width_, height_, fps_);
        if (failure.empty()) {
            std::lock_guard lock(mutex_);
            encoder_ = writer.encoder;
        }
        opened.set_value(failure);
        if (failure.empty()) write(writer);
    });
    const std::string failure = outcome.get();
    if (!failure.empty()) {
        thread_.join();
        running_.store(false);
        std::error_code removeError;
        std::filesystem::remove(file_, removeError);
        return Error{ErrorCode::Io, failure};
    }
    spdlog::info("Recording: {} x {} at {} fps, pictures with {}", width_, height_, fps_, encoder());
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
        if (stopping_) return false;
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

void VideoRecorder::write(Writer& writer) {
    std::vector<std::int16_t> sound;
    std::deque<Picture> pictures;
    std::string failure;

    for (;;) {
        bool stopping = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20), [&] { return stopping_ || !pictures_.empty(); });
            stopping = stopping_;
            pictures.swap(pictures_);
            sound.swap(sound_);
        }
        if (failure.empty()) {
            writer.pending.insert(writer.pending.end(), sound.begin(), sound.end());
            // Without sound arriving, silence keeps the sound up with the clock; otherwise the file stalls.
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            const std::int64_t taken =
                writer.soundSamples + static_cast<std::int64_t>(writer.pending.size() / kSoundChannels);
            if (const std::int64_t silence = media::silenceToAdd(taken, elapsed, kSoundRate); silence > 0) {
                writer.pending.resize(writer.pending.size() + static_cast<std::size_t>(silence) * kSoundChannels, 0);
            }
            failure = writer.encodeSound(false);
        }
        sound.clear();
        for (Picture& picture : pictures) {
            if (failure.empty()) {
                // Timed by when the picture was on screen: a refresh the window missed leaves a gap, not a drift.
                const auto since = std::max<std::int64_t>(
                    0, std::chrono::duration_cast<std::chrono::microseconds>(picture.at - started_).count());
                const std::int64_t pts = av_rescale_q(since, AVRational{1, 1'000'000}, kPictureTimeBase);
                failure = writer.encodePicture(picture.pixels, pts);
                if (failure.empty()) summary_.log.add(picture.kind);
            }
            std::lock_guard lock(mutex_);
            spare_.push_back(std::move(picture.pixels));
        }
        pictures.clear();
        if (stopping) break;
    }
    failure = writer.finish(failure);
    std::lock_guard lock(mutex_);
    error_ = failure;
    summary_.seconds = static_cast<double>(writer.soundSamples) / kSoundRate;
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

} // namespace veloplay::linuxos
