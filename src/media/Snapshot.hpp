#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/Result.hpp"

namespace veloplay::media {

/// A picture in memory, four bytes per pixel in the order blue, green, red,
/// alpha (as the swap chain holds it), rows one after the other without gaps.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

/// The picture as a PNG file, without its alpha channel: the screen has no transparency.
Result<std::vector<std::uint8_t>> encodePng(const Image& image);

/// "VeloPlay 2026-11-22 21-47-05.png": sorts by time, no characters Windows refuses in a name.
[[nodiscard]] std::string captureFileName(const std::tm& local, std::string_view extension);

/// A file in `folder` named by captureFileName() that does not exist yet, " (2)" and so on
/// after it if needed. Makes the folder if it is missing.
Result<std::filesystem::path> freeCapturePath(const std::filesystem::path& folder, const std::tm& local,
                                              std::string_view extension);

/// Writes `bytes` into `folder` under captureFileName(), made if missing. A file of that
/// name is never overwritten: a second one in the same second gets " (2)" and so on.
Result<std::filesystem::path> saveCapture(const std::filesystem::path& folder, const std::tm& local,
                                          std::string_view extension, const std::vector<std::uint8_t>& bytes);

} // namespace veloplay::media
