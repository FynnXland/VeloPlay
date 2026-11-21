#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "app/StreamProfile.hpp"
#include "media/VideoFrame.hpp"
#include "util/Result.hpp"

namespace veloplay::media {

/// Turns access units of the stream into pictures, using FFmpeg. With a
/// Direct3D 11 device it decodes on the graphics card (D3D11VA) and the
/// pictures stay there; without one, or if the card cannot decode the codec,
/// it decodes on the CPU.
class VideoDecoder {
public:
    struct Options {
        VideoCodec codec = VideoCodec::Hevc;
        void* d3d11Device = nullptr; // ID3D11Device*, optional
    };

    static Result<std::unique_ptr<VideoDecoder>> create(const Options& options);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Feeds one access unit (Annex B). The stream has no reordered frames, so
    /// a picture normally comes out for every one that goes in. An empty
    /// optional means the decoder needs more data, for example after the
    /// parameter sets.
    Result<std::optional<VideoFrame>> decode(std::span<const std::uint8_t> accessUnit);

    /// True once pictures come from the graphics card's decoder.
    [[nodiscard]] bool hardware() const;

    /// Short name for the display, for example "HEVC, D3D11VA".
    [[nodiscard]] std::string description() const;

private:
    struct Impl;
    explicit VideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace veloplay::media
