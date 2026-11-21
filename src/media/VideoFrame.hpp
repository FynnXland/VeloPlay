#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace veloplay::media {

enum class PixelLayout {
    I420,      // three planes in CPU memory: Y, U, V
    Nv12,      // two planes in CPU memory: Y, interleaved UV
    D3D11Nv12, // one slice of an NV12 texture array on the graphics card
};

struct ColorInfo {
    bool bt601 = false;     // false = BT.709, which is what the console sends for 1080p
    bool fullRange = false; // false = 16..235 for luma

    friend bool operator==(const ColorInfo&, const ColorInfo&) = default;
};

/// One decoded picture. Cheap to copy: the pixel data belongs to `owner`.
struct VideoFrame {
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::I420;
    std::array<const std::uint8_t*, 3> planes{};
    std::array<int, 3> strides{};
    void* d3d11Texture = nullptr; // ID3D11Texture2D*, for D3D11Nv12
    int d3d11Index = 0;           // slice of the texture array
    ColorInfo color{};
    std::uint64_t sequence = 0;   // counts decoded frames, starting at 1
    std::shared_ptr<void> owner;  // keeps the decoder's buffers alive
};

/// Coefficients that turn sampled Y, U, V (each 0..1) into RGB:
/// rgb[i] = rows[i][0]*y + rows[i][1]*u + rows[i][2]*v + rows[i][3].
struct ColorMatrix {
    std::array<std::array<float, 4>, 3> rows{};
};

ColorMatrix yuvToRgb(const ColorInfo& color);

} // namespace veloplay::media
