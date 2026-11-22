#include <ctime>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <miniz.h>

#include "media/Snapshot.hpp"

using namespace veloplay;

namespace {

std::tm someMoment() {
    std::tm moment{};
    moment.tm_year = 2026 - 1900;
    moment.tm_mon = 10;
    moment.tm_mday = 22;
    moment.tm_hour = 21;
    moment.tm_min = 7;
    moment.tm_sec = 5;
    return moment;
}

} // namespace

TEST(Snapshot, PacksThePictureAsPngWithItsColoursInTheRightOrder) {
    media::Image image;
    image.width = 3;
    image.height = 2;
    // Blue, green, red, alpha per pixel: a red, a green and a blue pixel, then three grey ones.
    image.bgra = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 90, 90, 90, 255, 90, 90, 90, 255, 90, 90, 90, 255};
    const auto png = media::encodePng(image);
    ASSERT_TRUE(png.ok()) << png.error().message;
    ASSERT_GT(png.value().size(), 8u);
    const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    EXPECT_TRUE(std::equal(signature, signature + 8, png.value().begin()));
    // The size sits in the header chunk, big-endian, right after its name.
    EXPECT_EQ(png.value()[16 + 3], 3);
    EXPECT_EQ(png.value()[20 + 3], 2);

    // Unpacking the image data gives the rows back, each led by its filter byte, as red, green, blue.
    std::size_t at = 8;
    std::vector<std::uint8_t> packed;
    while (at + 8 <= png.value().size()) {
        const std::size_t length = (std::size_t(png.value()[at]) << 24) | (std::size_t(png.value()[at + 1]) << 16) |
                                   (std::size_t(png.value()[at + 2]) << 8) | std::size_t(png.value()[at + 3]);
        const std::string type(png.value().begin() + static_cast<long>(at) + 4, png.value().begin() + static_cast<long>(at) + 8);
        if (type == "IDAT") packed.insert(packed.end(), png.value().begin() + static_cast<long>(at) + 8,
                                          png.value().begin() + static_cast<long>(at + 8 + length));
        at += 12 + length;
    }
    std::vector<std::uint8_t> rows(2 * (1 + 3 * 3));
    mz_ulong size = rows.size();
    ASSERT_EQ(mz_uncompress(rows.data(), &size, packed.data(), static_cast<mz_ulong>(packed.size())), MZ_OK);
    ASSERT_EQ(size, rows.size());
    const std::vector<std::uint8_t> first(rows.begin() + 1, rows.begin() + 10);
    EXPECT_EQ(first, (std::vector<std::uint8_t>{255, 0, 0, 0, 255, 0, 0, 0, 255}));
}

TEST(Snapshot, RefusesAPictureWhoseSizeDoesNotMatchItsPixels) {
    media::Image image;
    image.width = 4;
    image.height = 4;
    image.bgra.assign(10, 0);
    EXPECT_FALSE(media::encodePng(image).ok());
    EXPECT_FALSE(media::encodePng(media::Image{}).ok());
}

TEST(Snapshot, NamesFilesByTheMomentAndNeverOverwritesOne) {
    EXPECT_EQ(media::captureFileName(someMoment(), "png"), "VeloPlay 2026-11-22 21-07-05.png");
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "veloplay_tests" / "captures";
    std::filesystem::remove_all(folder);
    const std::vector<std::uint8_t> one = {1, 2, 3};
    const std::vector<std::uint8_t> two = {4, 5};
    const auto first = media::saveCapture(folder, someMoment(), "png", one);
    const auto second = media::saveCapture(folder, someMoment(), "png", two);
    ASSERT_TRUE(first.ok() && second.ok());
    EXPECT_EQ(first.value().filename(), "VeloPlay 2026-11-22 21-07-05.png");
    EXPECT_EQ(second.value().filename(), "VeloPlay 2026-11-22 21-07-05 (2).png");
    EXPECT_EQ(std::filesystem::file_size(first.value()), 3u);
    EXPECT_EQ(std::filesystem::file_size(second.value()), 2u);
}
