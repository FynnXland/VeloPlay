#include "media/VideoDecoder.hpp"

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace veloplay::media {

#ifdef _WIN32
// D3D11VaDevice.cpp: wraps an existing ID3D11Device for FFmpeg. Null if the
// device cannot decode video at all.
AVBufferRef* createD3D11VaDevice(void* d3d11Device);
#endif

namespace {

// Access units arrive without the zeroed tail FFmpeg's bitstream readers
// expect, so they are copied into a padded buffer first.
constexpr std::size_t kMaxAccessUnitSize = 16u * 1024u * 1024u;

std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

AVPixelFormat chooseFormat(AVCodecContext* context, const AVPixelFormat* formats) {
    // Take the graphics card's format when a device was attached and FFmpeg
    // offers it. If setting it up fails, FFmpeg asks again without it.
    if (context->hw_device_ctx) {
        for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
            if (*f == AV_PIX_FMT_D3D11) return *f;
        }
    }
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_YUV420P || *f == AV_PIX_FMT_YUVJ420P || *f == AV_PIX_FMT_NV12) return *f;
    }
    return formats[0];
}

} // namespace

struct VideoDecoder::Impl {
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    VideoCodec codec = VideoCodec::Hevc;
    bool hardwareAttached = false;
    bool hardwareSeen = false;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> padded;

    ~Impl() {
        av_packet_free(&packet);
        avcodec_free_context(&context);
    }
};

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VideoDecoder::~VideoDecoder() = default;

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::create(const Options& options) {
    // FFmpeg prints to stderr on its own. Failures come back as return codes
    // and are reported from here, so its chatter about lost packets stays out.
    static std::once_flag quiet;
    std::call_once(quiet, [] { av_log_set_level(AV_LOG_FATAL); });

    const AVCodecID id = options.codec == VideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) return Error{ErrorCode::InvalidArgument, "FFmpeg was built without this decoder"};

    auto impl = std::make_unique<Impl>();
    impl->codec = options.codec;
    impl->context = avcodec_alloc_context3(codec);
    impl->packet = av_packet_alloc();
    if (!impl->context || !impl->packet) return Error{ErrorCode::Io, "out of memory"};

    AVCodecContext* context = impl->context;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->get_format = &chooseFormat;
    // Slice threads split one picture; frame threads would hold pictures back
    // by one per thread, which is delay we cannot afford.
    context->thread_type = FF_THREAD_SLICE;
    context->thread_count = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));

#ifdef _WIN32
    if (options.d3d11Device) {
        if (AVBufferRef* device = createD3D11VaDevice(options.d3d11Device)) {
            context->hw_device_ctx = device; // the context owns the reference now
            // Decoded pictures wait a little before they are drawn (a few in the pipeline, a few in the
            // window's queue in front of the renderer), each holding a surface of the decoder's pool.
            context->extra_hw_frames = 12;
            impl->hardwareAttached = true;
        }
    }
#else
    (void)options.d3d11Device;
#endif

    const int opened = avcodec_open2(context, codec, nullptr);
    if (opened < 0) return Error{ErrorCode::Io, "decoder could not be opened: " + errorText(opened)};
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

Result<std::optional<VideoFrame>> VideoDecoder::decode(std::span<const std::uint8_t> accessUnit) {
    Impl& impl = *impl_;
    if (accessUnit.empty() || accessUnit.size() > kMaxAccessUnitSize) {
        return Error{ErrorCode::InvalidArgument, "access unit has an impossible size"};
    }
    impl.padded.assign(accessUnit.begin(), accessUnit.end());
    impl.padded.resize(accessUnit.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    impl.packet->data = impl.padded.data();
    impl.packet->size = static_cast<int>(accessUnit.size());

    const int sent = avcodec_send_packet(impl.context, impl.packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        return Error{ErrorCode::InvalidData, "access unit was rejected: " + errorText(sent)};
    }

    // Keep the newest picture if the decoder hands out more than one.
    std::optional<VideoFrame> newest;
    while (true) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return Error{ErrorCode::Io, "out of memory"};
        std::shared_ptr<void> owner(frame, [](void* p) {
            AVFrame* f = static_cast<AVFrame*>(p);
            av_frame_free(&f);
        });
        const int received = avcodec_receive_frame(impl.context, frame);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
        if (received < 0) return Error{ErrorCode::InvalidData, "decoding failed: " + errorText(received)};

        VideoFrame out;
        out.width = frame->width;
        out.height = frame->height;
        out.color.bt601 = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M;
        out.color.fullRange = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;

        if (frame->format == AV_PIX_FMT_D3D11) {
            const auto* frames = frame->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data)
                                                      : nullptr;
            if (!frames || frames->sw_format != AV_PIX_FMT_NV12) {
                return Error{ErrorCode::InvalidData, "the graphics card decoded to a format other than NV12"};
            }
            out.layout = PixelLayout::D3D11Nv12;
            out.d3d11Texture = frame->data[0];
            out.d3d11Index = static_cast<int>(reinterpret_cast<std::intptr_t>(frame->data[1]));
            impl.hardwareSeen = true;
        } else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            out.layout = PixelLayout::I420;
            for (int i = 0; i < 3; ++i) {
                out.planes[static_cast<std::size_t>(i)] = frame->data[i];
                out.strides[static_cast<std::size_t>(i)] = frame->linesize[i];
            }
        } else if (frame->format == AV_PIX_FMT_NV12) {
            out.layout = PixelLayout::Nv12;
            for (int i = 0; i < 2; ++i) {
                out.planes[static_cast<std::size_t>(i)] = frame->data[i];
                out.strides[static_cast<std::size_t>(i)] = frame->linesize[i];
            }
        } else {
            return Error{ErrorCode::InvalidData, "the stream uses a pixel format that is not supported yet (10 bit?)"};
        }
        out.sequence = ++impl.sequence;
        out.owner = std::move(owner);
        newest = std::move(out);
    }
    return newest;
}

bool VideoDecoder::hardware() const { return impl_->hardwareSeen; }

std::string VideoDecoder::description() const {
    std::string text = impl_->codec == VideoCodec::H264 ? "H.264" : "HEVC";
    text += impl_->hardwareSeen ? ", D3D11VA" : ", CPU";
    return text;
}

} // namespace veloplay::media
