#include "media/Snapshot.hpp"

#include <cstdio>
#include <fstream>
#include <system_error>

#include <miniz.h>

namespace veloplay::media {

Result<std::vector<std::uint8_t>> encodePng(const Image& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.bgra.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return Error{ErrorCode::InvalidArgument, "the picture has no pixels or not as many as its size says"};
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
    for (std::size_t from = 0, to = 0; from < image.bgra.size(); from += 4, to += 3) {
        rgb[to] = image.bgra[from + 2];
        rgb[to + 1] = image.bgra[from + 1];
        rgb[to + 2] = image.bgra[from];
    }
    std::size_t length = 0;
    // Level 6: a 1080p picture takes a fraction of a second and comes out at a few megabytes.
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), image.width, image.height, 3, &length, 6, MZ_FALSE);
    if (!png) return Error{ErrorCode::Io, "the picture could not be packed as PNG"};
    const auto* bytes = static_cast<const std::uint8_t*>(png);
    std::vector<std::uint8_t> out(bytes, bytes + length);
    mz_free(png);
    return out;
}

std::string captureFileName(const std::tm& local, std::string_view extension) {
    char name[64];
    std::snprintf(name, sizeof(name), "VeloPlay %04d-%02d-%02d %02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return std::string(name) + "." + std::string(extension);
}

Result<std::filesystem::path> saveCapture(const std::filesystem::path& folder, const std::tm& local,
                                          std::string_view extension, const std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return Error{ErrorCode::Io, "could not create the folder for pictures: " + error.message()};
    const std::string name = captureFileName(local, extension);
    const std::string stem = name.substr(0, name.size() - extension.size() - 1);
    std::filesystem::path file = folder / name;
    for (int number = 2; std::filesystem::exists(file, error) && number < 1000; ++number) {
        file = folder / (stem + " (" + std::to_string(number) + ")." + std::string(extension));
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) return Error{ErrorCode::Io, "could not write the picture"};
    return file;
}

} // namespace veloplay::media
