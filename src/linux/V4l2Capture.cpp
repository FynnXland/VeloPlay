#include "linux/V4l2Capture.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>

#include <spdlog/spdlog.h>

#include "app/CaptureMatch.hpp"

namespace veloplay::linuxos {

namespace {

constexpr int kFullHdArea = 1920 * 1080;
constexpr unsigned kBufferCount = 4; // enough that the card never waits for us, few enough to stay current
constexpr int kSoundRate = 48'000;
constexpr int kSoundChannels = 2;

int xioctl(int fd, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::string systemText(int code) { return std::strerror(code); }

/// What went wrong when opening, said so that the user can do something about it.
std::string openFailure(const std::string& path, int code) {
    switch (code) {
        case EACCES:
        case EPERM:
            return "no permission to open " + path + ": the user needs to be in the group \"video\" "
                   "(sudo usermod -aG video $USER, then log in again)";
        case ENOENT:
        case ENODEV:
        case ENXIO: return path + " is gone; the capture card was unplugged";
        case EBUSY: return path + " is in use by another program";
        default: return "opening " + path + " failed (" + systemText(code) + ")";
    }
}

std::uint32_t capabilitiesOf(const v4l2_capability& capability) {
    // A node of a device with several (picture and metadata) names its own capabilities separately.
    return (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
}

bool isPictureNode(const v4l2_capability& capability) {
    const std::uint32_t caps = capabilitiesOf(capability);
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

std::string cardName(const v4l2_capability& capability) {
    const auto* text = reinterpret_cast<const char*>(capability.card);
    return std::string(text, strnlen(text, sizeof(capability.card)));
}

/// The most pictures per second up to 60 the device gives at this size; 0 when it does not say.
double fpsFor(int fd, std::uint32_t format, int width, int height) {
    double best = 0.0;
    double smallest = 0.0;
    for (std::uint32_t index = 0;; ++index) {
        v4l2_frmivalenum interval{};
        interval.index = index;
        interval.pixel_format = format;
        interval.width = static_cast<std::uint32_t>(width);
        interval.height = static_cast<std::uint32_t>(height);
        if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) break;
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (interval.discrete.numerator == 0) continue;
            const double fps = static_cast<double>(interval.discrete.denominator) / interval.discrete.numerator;
            if (fps <= 60.5) best = std::max(best, fps);
            if (smallest == 0.0 || fps < smallest) smallest = fps;
        } else {
            // A range: its shortest interval is its most pictures, of which 60 are enough.
            if (interval.stepwise.min.numerator == 0) break;
            const double most =
                static_cast<double>(interval.stepwise.min.denominator) / interval.stepwise.min.numerator;
            best = std::min(most, 60.0);
            break;
        }
    }
    // Only faster than 60 on offer: the slowest of them, it is asked for 60 anyway.
    return best > 0.0 ? best : smallest;
}

std::vector<CaptureMode> offeredModes(int fd) {
    std::vector<CaptureMode> modes;
    for (std::uint32_t index = 0;; ++index) {
        v4l2_fmtdesc description{};
        description.index = index;
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (!readsCaptureFormat(description.pixelformat)) continue;
        for (std::uint32_t sizeIndex = 0;; ++sizeIndex) {
            v4l2_frmsizeenum size{};
            size.index = sizeIndex;
            size.pixel_format = description.pixelformat;
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
            int width = 0;
            int height = 0;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                width = static_cast<int>(size.discrete.width);
                height = static_cast<int>(size.discrete.height);
            } else {
                // A range of sizes: the largest up to 1080p that fits its steps.
                const auto& range = size.stepwise;
                const auto fit = [](std::uint32_t low, std::uint32_t high, std::uint32_t step, std::uint32_t wanted) {
                    const std::uint32_t top = std::clamp(wanted, low, std::max(low, high));
                    return static_cast<int>(step > 0 ? low + (top - low) / step * step : top);
                };
                width = fit(range.min_width, range.max_width, range.step_width, 1920);
                height = fit(range.min_height, range.max_height, range.step_height, 1080);
            }
            if (width > 0 && height > 0) {
                modes.push_back({description.pixelformat, width, height,
                                 fpsFor(fd, description.pixelformat, width, height)});
            }
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
        }
    }
    return modes;
}

std::string fourcc(std::uint32_t format) {
    std::string text(4, ' ');
    for (int i = 0; i < 4; ++i) text[static_cast<std::size_t>(i)] = static_cast<char>((format >> (8 * i)) & 0xff);
    return text;
}

const int* coefficientsFor(bool bt601) { return sws_getCoefficients(bt601 ? SWS_CS_ITU601 : SWS_CS_ITU709); }

} // namespace

// ---- Listing ----------------------------------------------------------------

std::vector<CaptureDevice> listCaptureDevices(std::string* why, const std::filesystem::path& folder) {
    std::vector<CaptureDevice> out;
    if (why) why->clear();
    std::vector<std::pair<int, std::filesystem::path>> nodes;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() <= 5 || name.compare(0, 5, "video") != 0) continue;
        if (!std::all_of(name.begin() + 5, name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        if (name.size() > 9) continue; // more digits than any machine has devices
        nodes.emplace_back(std::stoi(name.substr(5)), entry.path());
    }
    std::sort(nodes.begin(), nodes.end());
    std::size_t denied = 0;
    for (const auto& [number, path] : nodes) {
        (void)number;
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM) ++denied;
            continue;
        }
        v4l2_capability capability{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 && isPictureNode(capability)) {
            out.push_back({path.string(), cardName(capability)});
        }
        ::close(fd);
    }
    if (out.empty() && denied > 0 && why) {
        *why = std::to_string(denied) + " video devices may not be opened: the user needs to be in the group \"video\"";
    }
    return out;
}

// ---- Pure parts ---------------------------------------------------------------

bool readsCaptureFormat(std::uint32_t format) {
    return format == V4L2_PIX_FMT_NV12 || format == V4L2_PIX_FMT_YUYV || format == V4L2_PIX_FMT_YUV420 ||
           format == V4L2_PIX_FMT_MJPEG || format == V4L2_PIX_FMT_JPEG;
}

std::optional<CaptureMode> chooseCaptureMode(const std::vector<CaptureMode>& offered) {
    std::optional<CaptureMode> best;
    double bestScore = -1.0;
    for (const CaptureMode& mode : offered) {
        if (!readsCaptureFormat(mode.format) || mode.width <= 0 || mode.height <= 0) continue;
        const double area = static_cast<double>(mode.width) * mode.height;
        // Above 1080p counts a little less than 1080p itself: a 4K picture costs far more to decode and draw.
        const double counted = area > kFullHdArea ? kFullHdArea - 1.0 : area;
        const bool compressed = mode.format == V4L2_PIX_FMT_MJPEG || mode.format == V4L2_PIX_FMT_JPEG;
        const double score = counted * 1000.0 + std::min(mode.fps, 61.0) * 10.0 + (compressed ? 0.0 : 1.0) +
                             (mode.format == V4L2_PIX_FMT_NV12 ? 0.5 : 0.0);
        if (score > bestScore) {
            bestScore = score;
            best = mode;
        }
    }
    return best;
}

media::ColorInfo captureColor(std::uint32_t colorspace, std::uint32_t ycbcrEncoding, std::uint32_t quantization,
                              int height) {
    media::ColorInfo color;
    if (colorspace == V4L2_COLORSPACE_DEFAULT && ycbcrEncoding == V4L2_YCBCR_ENC_DEFAULT &&
        quantization == V4L2_QUANTIZATION_DEFAULT) {
        color.bt601 = height < 720;
        color.fullRange = false;
        return color;
    }
    const std::uint32_t encoding = ycbcrEncoding == V4L2_YCBCR_ENC_DEFAULT
                                       ? static_cast<std::uint32_t>(V4L2_MAP_YCBCR_ENC_DEFAULT(colorspace))
                                       : ycbcrEncoding;
    const std::uint32_t range = quantization == V4L2_QUANTIZATION_DEFAULT
                                    ? static_cast<std::uint32_t>(V4L2_MAP_QUANTIZATION_DEFAULT(false, colorspace, encoding))
                                    : quantization;
    // The pipeline knows two matrices; everything but BT.709 is closest to BT.601.
    color.bt601 = encoding != V4L2_YCBCR_ENC_709 && encoding != V4L2_YCBCR_ENC_XV709;
    color.fullRange = range == V4L2_QUANTIZATION_FULL_RANGE;
    return color;
}

void yuyvToNv12(const std::uint8_t* yuyv, int yuyvStride, int width, int height, std::uint8_t* luma, int lumaStride,
                std::uint8_t* chroma, int chromaStride) {
    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* top = yuyv + static_cast<std::ptrdiff_t>(y) * yuyvStride;
        // An odd last row has no partner; its colour stands alone.
        const std::uint8_t* bottom = y + 1 < height ? top + yuyvStride : top;
        std::uint8_t* lumaTop = luma + static_cast<std::ptrdiff_t>(y) * lumaStride;
        std::uint8_t* lumaBottom = lumaTop + lumaStride;
        std::uint8_t* colour = chroma + static_cast<std::ptrdiff_t>(y / 2) * chromaStride;
        for (int x = 0; x < width; x += 2) {
            const std::uint8_t* a = top + x * 2;
            const std::uint8_t* b = bottom + x * 2;
            lumaTop[x] = a[0];
            lumaTop[x + 1] = a[2];
            if (y + 1 < height) {
                lumaBottom[x] = b[0];
                lumaBottom[x + 1] = b[2];
            }
            colour[x] = static_cast<std::uint8_t>((a[1] + b[1] + 1) / 2);
            colour[x + 1] = static_cast<std::uint8_t>((a[3] + b[3] + 1) / 2);
        }
    }
}

// ---- MjpegDecoder -------------------------------------------------------------

MjpegDecoder::~MjpegDecoder() {
    sws_freeContext(scale_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&context_);
}

Result<media::VideoFrame> MjpegDecoder::decode(std::span<const std::uint8_t> jpeg) {
    if (!context_) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!codec) return Error{ErrorCode::Io, "this FFmpeg cannot decode MJPEG"};
        context_ = avcodec_alloc_context3(codec);
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!context_ || !packet_ || !frame_) return Error{ErrorCode::Io, "no memory for the MJPEG decoder"};
        // One picture in, the same picture out: threads over frames would hold pictures back.
        context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context_->thread_type = FF_THREAD_SLICE;
        if (avcodec_open2(context_, codec, nullptr) < 0) {
            avcodec_free_context(&context_);
            return Error{ErrorCode::Io, "opening the MJPEG decoder failed"};
        }
    }
    if (jpeg.empty()) return Error{ErrorCode::InvalidData, "an empty picture"};
    packet_->data = const_cast<std::uint8_t*>(jpeg.data()); // not reference counted: the decoder copies what it keeps
    packet_->size = static_cast<int>(jpeg.size());
    const int sent = avcodec_send_packet(context_, packet_);
    packet_->data = nullptr;
    packet_->size = 0;
    if (sent < 0) return Error{ErrorCode::InvalidData, "the picture from the card is no JPEG"};
    if (avcodec_receive_frame(context_, frame_) < 0) return Error{ErrorCode::InvalidData, "the JPEG gave no picture"};

    const int width = frame_->width;
    const int height = frame_->height;
    const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format));
    media::VideoFrame picture;
    picture.width = width;
    picture.height = height;
    // A JPEG is full range unless it says otherwise, and BT.601 unless it says BT.709.
    picture.color.fullRange = frame_->color_range != AVCOL_RANGE_MPEG;
    picture.color.bt601 = frame_->colorspace != AVCOL_SPC_BT709;
    const bool planar420 = description && description->nb_components == 3 &&
                           !(description->flags & AV_PIX_FMT_FLAG_RGB) && (description->flags & AV_PIX_FMT_FLAG_PLANAR) &&
                           description->log2_chroma_w == 1 && description->log2_chroma_h == 1 &&
                           description->comp[0].depth == 8;
    if (planar420) {
        // Already 4:2:0: handed on as it is, the decoder's buffers kept alive by the picture.
        AVFrame* kept = av_frame_clone(frame_);
        av_frame_unref(frame_);
        if (!kept) return Error{ErrorCode::Io, "no memory for a picture"};
        picture.layout = media::PixelLayout::I420;
        for (int plane = 0; plane < 3; ++plane) {
            picture.planes[static_cast<std::size_t>(plane)] = kept->data[plane];
            picture.strides[static_cast<std::size_t>(plane)] = kept->linesize[plane];
        }
        picture.owner = std::shared_ptr<AVFrame>(kept, [](AVFrame* frame) { av_frame_free(&frame); });
        return picture;
    }
    // 4:2:2 (what most cards send) and the rest: to NV12, in the range of the JPEG, so nothing is squeezed.
    scale_ = sws_getCachedContext(scale_, width, height, static_cast<AVPixelFormat>(frame_->format), width, height,
                                  AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scale_) {
        av_frame_unref(frame_);
        return Error{ErrorCode::Io, "no conversion for the JPEG's pixel format"};
    }
    const int range = picture.color.fullRange ? 1 : 0;
    sws_setColorspaceDetails(scale_, coefficientsFor(picture.color.bt601), range, coefficientsFor(picture.color.bt601),
                             range, 0, 1 << 16, 1 << 16);
    const int stride = (width + 1) & ~1;
    const std::size_t lumaSize = static_cast<std::size_t>(stride) * height;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(lumaSize + static_cast<std::size_t>(stride) * ((height + 1) / 2));
    std::uint8_t* planes[4] = {pixels->data(), pixels->data() + lumaSize, nullptr, nullptr};
    const int strides[4] = {stride, stride, 0, 0};
    sws_scale(scale_, frame_->data, frame_->linesize, 0, height, planes, strides);
    av_frame_unref(frame_);
    picture.layout = media::PixelLayout::Nv12;
    picture.planes = {planes[0], planes[1], nullptr};
    picture.strides = {stride, stride, 0};
    picture.owner = pixels;
    return picture;
}

// ---- V4l2Source ---------------------------------------------------------------

V4l2Source::~V4l2Source() { close(); }

std::string V4l2Source::failure() {
    std::lock_guard lock(mutex_);
    return failure_;
}

Result<void> V4l2Source::openDevice(const std::string& path, const std::string& name) {
    if (fd_ >= 0) return Error{ErrorCode::InvalidArgument, "a device is already open"};
    path_ = path;
    // Made before anything can wait, so stop() always has a way to end the wait.
    if (wakeFd_ < 0) wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeFd_ < 0) return Error{ErrorCode::Io, "no eventfd (" + systemText(errno) + ")"};
    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return Error{ErrorCode::Io, openFailure(path, errno)};
    v4l2_capability capability{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) return Error{ErrorCode::Io, path + " is no video device"};
    if (!isPictureNode(capability)) return Error{ErrorCode::Io, path + " delivers no pictures"};
    if (const auto set = setUpVideo(); !set) return set.error();
    // The card's sound is a device of its own; found by its name. Without it the picture still runs.
    openSound(name.empty() ? cardName(capability) : name);
    return {};
}

Result<void> V4l2Source::setUpVideo() {
    const std::optional<CaptureMode> mode = chooseCaptureMode(offeredModes(fd_));
    if (!mode) {
        return Error{ErrorCode::Io, "the capture card offers no picture format VeloPlay reads (NV12, YUYV, I420, MJPEG)"};
    }
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<std::uint32_t>(mode->width);
    format.fmt.pix.height = static_cast<std::uint32_t>(mode->height);
    format.fmt.pix.pixelformat = mode->format;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
        if (errno == EBUSY) return Error{ErrorCode::Io, path_ + " is in use by another program"};
        return Error{ErrorCode::Io, "setting the picture format failed (" + systemText(errno) + ")"};
    }
    // What the driver gave, which can differ from what was asked.
    if (xioctl(fd_, VIDIOC_G_FMT, &format) < 0) {
        return Error{ErrorCode::Io, "reading the picture format failed (" + systemText(errno) + ")"};
    }
    const v4l2_pix_format& pix = format.fmt.pix;
    format_ = pix.pixelformat;
    width_ = static_cast<int>(pix.width);
    height_ = static_cast<int>(pix.height);
    if (!readsCaptureFormat(format_)) return Error{ErrorCode::Io, "the card gave the format " + fourcc(format_)};
    if (width_ < 2 || height_ < 2) return Error{ErrorCode::Io, "the picture has no size"};
    if ((format_ == V4L2_PIX_FMT_YUYV || format_ == V4L2_PIX_FMT_NV12 || format_ == V4L2_PIX_FMT_YUV420) && width_ % 2) {
        return Error{ErrorCode::Io, "the picture has an odd width"};
    }
    const int packed = format_ == V4L2_PIX_FMT_YUYV ? width_ * 2 : width_;
    stride_ = std::max(static_cast<int>(pix.bytesperline), packed);
    imageSize_ = pix.sizeimage;
    // Colour details count only where the driver says it filled them in.
    const bool extended = pix.priv == V4L2_PIX_FMT_PRIV_MAGIC;
    color_ = captureColor(pix.colorspace, extended ? pix.ycbcr_enc : 0u, extended ? pix.quantization : 0u, height_);
    if (pix.field != V4L2_FIELD_NONE && pix.field != V4L2_FIELD_ANY) {
        spdlog::warn("Capture: the card sends fields ({}), the picture may show combing", pix.field);
    }

    if (mode->fps > 0.0) {
        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_G_PARM, &parameters) == 0 &&
            (parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
            parameters.parm.capture.timeperframe.numerator = 1000;
            parameters.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(std::lround(std::min(mode->fps, 60.0) * 1000));
            xioctl(fd_, VIDIOC_S_PARM, &parameters); // a card that keeps its own rate still delivers
        }
    }

    v4l2_requestbuffers request{};
    request.count = kBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0) {
        if (errno == EBUSY) return Error{ErrorCode::Io, path_ + " is in use by another program"};
        return Error{ErrorCode::Io, "the card has no streaming buffers (" + systemText(errno) + ")"};
    }
    if (request.count < 2) return Error{ErrorCode::Io, "the card gave too few buffers"};
    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) return Error{ErrorCode::Io, "asking for a buffer failed"};
        void* data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
        if (data == MAP_FAILED) return Error{ErrorCode::Io, "mapping a buffer failed (" + systemText(errno) + ")"};
        buffers_.push_back({data, buffer.length});
        if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) return Error{ErrorCode::Io, "queueing a buffer failed"};
    }
    spdlog::info("Capture: picture {} x {} {} at {:.2f} fps, rows of {} bytes, {}{}", width_, height_, fourcc(format_),
                 mode->fps, stride_, color_.bt601 ? "BT.601" : "BT.709", color_.fullRange ? ", full range" : "");
    return {};
}

void V4l2Source::openSound(const std::string& name) {
    // SDL's counts of who uses a subsystem are not safe across threads, and this runs on the opener's
    // thread: the window starts SDL's audio on the main thread, here it is only used.
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        spdlog::info("Capture card: no sound, SDL's audio is not started");
        return;
    }
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
    std::vector<std::string> names;
    for (int i = 0; devices && i < count; ++i) {
        const char* deviceName = SDL_GetAudioDeviceName(devices[i]);
        names.emplace_back(deviceName ? deviceName : "");
    }
    const int match = pickSoundFor(name, names);
    const SDL_AudioDeviceID device = match >= 0 ? devices[match] : 0;
    SDL_free(devices);
    if (match < 0) {
        spdlog::info("Capture card: no sound input found that belongs to {}", name);
        return;
    }
    // SDL converts to what the sound output and a recording like best.
    const SDL_AudioSpec wanted{SDL_AUDIO_S16, kSoundChannels, kSoundRate};
    sound_ = SDL_OpenAudioDeviceStream(device, &wanted, &V4l2Source::soundArrived, this);
    if (!sound_) {
        spdlog::warn("Capture card: sound input {} could not be opened: {}", names[static_cast<std::size_t>(match)],
                     SDL_GetError());
        return;
    }
    spdlog::info("Capture card: sound from {}", names[static_cast<std::size_t>(match)]);
}

void V4l2Source::soundArrived(void* self, SDL_AudioStream* stream, int, int) {
    auto* source = static_cast<V4l2Source*>(self);
    const int available = SDL_GetAudioStreamAvailable(stream);
    if (available <= 0) return;
    source->soundBuffer_.resize(static_cast<std::size_t>(available) / sizeof(std::int16_t));
    const int got = SDL_GetAudioStreamData(stream, source->soundBuffer_.data(),
                                           static_cast<int>(source->soundBuffer_.size() * sizeof(std::int16_t)));
    // Read even without a sink, so nothing piles up in the stream.
    session::IMediaSink* sink = source->sink_.load();
    if (got > 0 && sink) {
        sink->onPcm(std::span<const std::int16_t>(source->soundBuffer_.data(),
                                                  static_cast<std::size_t>(got) / sizeof(std::int16_t)));
    }
}

void V4l2Source::start(session::IMediaSink* sink) {
    if (fd_ < 0 || thread_.joinable()) return;
    ended_.store(false);
    // The format first, so the first sound finds the output ready for it.
    if (sound_ && sink) sink->onPcmFormat(kSoundRate, kSoundChannels);
    sink_.store(sink);
    if (sound_) SDL_ResumeAudioStreamDevice(sound_);
    thread_ = std::thread([this] { run(); });
}

void V4l2Source::stop() {
    if (thread_.joinable()) {
        const std::uint64_t one = 1;
        if (wakeFd_ >= 0) (void)!::write(wakeFd_, &one, sizeof(one));
        thread_.join();
    }
    if (sound_) {
        // After this no callback runs any more, so the sink is safe from here on.
        SDL_DestroyAudioStream(sound_);
        sound_ = nullptr;
    }
    sink_.store(nullptr);
    if (streaming_) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
}

void V4l2Source::close() {
    stop();
    for (const Mapped& buffer : buffers_) munmap(buffer.data, buffer.length);
    buffers_.clear();
    if (fd_ >= 0) {
        v4l2_requestbuffers none{};
        none.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        none.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &none);
        ::close(fd_);
        fd_ = -1;
    }
    if (wakeFd_ >= 0) {
        ::close(wakeFd_);
        wakeFd_ = -1;
    }
}

void V4l2Source::run() {
    const auto fail = [this](std::string text) {
        std::lock_guard lock(mutex_);
        failure_ = std::move(text);
    };
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        fail(errno == ENODEV ? path_ + " is gone; the capture card was unplugged"
                             : "starting the capture card failed (" + systemText(errno) + ")");
        ended_.store(true);
        return;
    }
    streaming_ = true;
    std::size_t transient = 0;
    for (;;) {
        pollfd waits[2] = {{fd_, POLLIN, 0}, {wakeFd_, POLLIN, 0}};
        // A card without a signal on its input sends nothing for as long as that lasts; that is no error.
        const int ready = poll(waits, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            fail("waiting for the capture card failed (" + systemText(errno) + ")");
            break;
        }
        if (waits[1].revents) break;
        if (ready == 0 || waits[0].revents == 0) continue;
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) continue;
            // EIO is the driver's way to say a picture went wrong, signal loss included; the next may be fine.
            if (errno == EIO && ++transient < 1000) continue;
            fail(errno == ENODEV ? path_ + " is gone; the capture card was unplugged"
                                 : "reading from the capture card failed (" + systemText(errno) + ")");
            break;
        }
        if (buffer.index < buffers_.size() && !(buffer.flags & V4L2_BUF_FLAG_ERROR) && buffer.bytesused > 0) {
            const Mapped& mapped = buffers_[buffer.index];
            handOn(static_cast<const std::uint8_t*>(mapped.data), std::min<std::size_t>(buffer.bytesused, mapped.length));
        }
        if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            fail(errno == ENODEV ? path_ + " is gone; the capture card was unplugged"
                                 : "handing a buffer back to the card failed (" + systemText(errno) + ")");
            break;
        }
    }
    ended_.store(true);
}

void V4l2Source::handOn(const std::uint8_t* data, std::size_t length) {
    session::IMediaSink* sink = sink_.load();
    if (!sink) return;
    const auto now = std::chrono::steady_clock::now();
    media::VideoFrame picture;
    picture.width = width_;
    picture.height = height_;
    picture.color = color_;
    const auto stride = static_cast<std::size_t>(stride_);
    const auto rows = static_cast<std::size_t>(height_);
    if (format_ == V4L2_PIX_FMT_NV12) {
        const std::size_t needed = stride * rows + stride * ((rows + 1) / 2);
        if (length < needed) return;
        // Copied: the card's buffer goes back to it right away.
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(data, data + needed);
        picture.layout = media::PixelLayout::Nv12;
        picture.planes = {pixels->data(), pixels->data() + stride * rows, nullptr};
        picture.strides = {stride_, stride_, 0};
        picture.owner = pixels;
    } else if (format_ == V4L2_PIX_FMT_YUV420) {
        const std::size_t half = stride / 2;
        const std::size_t chromaRows = (rows + 1) / 2;
        const std::size_t needed = stride * rows + half * chromaRows * 2;
        if (length < needed) return;
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(data, data + needed);
        picture.layout = media::PixelLayout::I420;
        std::uint8_t* base = pixels->data();
        picture.planes = {base, base + stride * rows, base + stride * rows + half * chromaRows};
        picture.strides = {stride_, static_cast<int>(half), static_cast<int>(half)};
        picture.owner = pixels;
    } else if (format_ == V4L2_PIX_FMT_YUYV) {
        if (length < stride * rows) return;
        const auto width = static_cast<std::size_t>(width_);
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(width * rows + width * ((rows + 1) / 2));
        std::uint8_t* luma = pixels->data();
        std::uint8_t* chroma = luma + width * rows;
        yuyvToNv12(data, stride_, width_, height_, luma, width_, chroma, width_);
        picture.layout = media::PixelLayout::Nv12;
        picture.planes = {luma, chroma, nullptr};
        picture.strides = {width_, width_, 0};
        picture.owner = pixels;
    } else {
        auto decoded = mjpeg_.decode(std::span<const std::uint8_t>(data, length));
        if (!decoded) {
            // Cards send a broken picture now and then (a USB hiccup); the next one is whole again.
            if (undecodable_++ % 300 == 0) spdlog::warn("Capture: {} ({} so far)", decoded.error().message, undecodable_);
            return;
        }
        picture = std::move(decoded.value());
    }
    picture.receivedAt = now;
    if (sink->onPicture(std::move(picture))) pictures_.fetch_add(1);
}

// ---- CaptureSession -----------------------------------------------------------

CaptureSession::~CaptureSession() { stop(); }

Result<void> CaptureSession::start(const session::SessionConfig& config, session::IMediaSink* sink) {
    if (opener_.joinable()) return Error{ErrorCode::InvalidArgument, "already started"};
    countedAt_ = session::Clock::now();
    opener_ = std::thread([this, path = config.host, name = config.name, sink] {
        const auto opened = source_.openDevice(path, name);
        {
            std::lock_guard lock(mutex_);
            if (!opened) openFailure_ = opened.error().message;
            opened_ = true;
        }
        if (opened && !stopped_) source_.start(sink);
    });
    return {};
}

void CaptureSession::stop() {
    stopped_ = true;
    if (opener_.joinable()) opener_.join();
    source_.stop();
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
    if (failure.empty() && source_.ended()) {
        failure = source_.failure().empty() ? "the capture card stopped delivering" : source_.failure();
    }
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

} // namespace veloplay::linuxos
