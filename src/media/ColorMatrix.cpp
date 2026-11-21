#include "media/VideoFrame.hpp"

namespace veloplay::media {

ColorMatrix yuvToRgb(const ColorInfo& color) {
    const double kr = color.bt601 ? 0.299 : 0.2126;
    const double kb = color.bt601 ? 0.114 : 0.0722;
    const double kg = 1.0 - kr - kb;

    // Bring the samples to Y' in 0..1 and Cb, Cr in -0.5..0.5 first.
    const double yScale = color.fullRange ? 1.0 : 255.0 / 219.0;
    const double yOffset = color.fullRange ? 0.0 : -16.0 / 219.0;
    const double cScale = color.fullRange ? 1.0 : 255.0 / 224.0;
    const double cOffset = color.fullRange ? -0.5 : -128.0 / 224.0;

    const double rCr = 2.0 * (1.0 - kr);
    const double bCb = 2.0 * (1.0 - kb);
    const double gCb = -2.0 * kb * (1.0 - kb) / kg;
    const double gCr = -2.0 * kr * (1.0 - kr) / kg;

    const auto row = [&](double cb, double cr) {
        return std::array<float, 4>{static_cast<float>(yScale), static_cast<float>(cb * cScale),
                                    static_cast<float>(cr * cScale),
                                    static_cast<float>(yOffset + (cb + cr) * cOffset)};
    };
    ColorMatrix matrix;
    matrix.rows[0] = row(0.0, rCr);
    matrix.rows[1] = row(gCb, gCr);
    matrix.rows[2] = row(bCb, 0.0);
    return matrix;
}

} // namespace veloplay::media
