#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <miniz.h>

#include "setup/Payload.hpp"
#include "update/Zip.hpp"

using namespace veloplay;

namespace {

std::filesystem::path freshFolder(const char* name) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "veloplay_tests" / name;
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

/// The headers of a Windows program with two sections, as little as Windows asks for: "MZ", the place of
/// "PE", the file header, an optional header of the usual size, the section table, then the sections.
std::vector<std::uint8_t> smallProgram() {
    constexpr std::size_t pe = 0x80;
    constexpr std::size_t optionalHeader = 240;
    constexpr std::size_t table = pe + 24 + optionalHeader;
    std::vector<std::uint8_t> bytes(0x1000, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    put32(bytes, 0x3C, pe);
    bytes[pe] = 'P';
    bytes[pe + 1] = 'E';
    bytes[pe + 6] = 2; // sections
    bytes[pe + 20] = static_cast<std::uint8_t>(optionalHeader);
    // .text: 0x200 bytes at 0x400. .rsrc: 0x600 bytes at 0xA00, so the program ends at 0x1000. The second
    // entry comes first in the file on purpose: the order of the table says nothing.
    put32(bytes, table + 16, 0x600);
    put32(bytes, table + 20, 0xA00);
    put32(bytes, table + 40 + 16, 0x200);
    put32(bytes, table + 40 + 20, 0x400);
    return bytes;
}

void save(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::vector<std::uint8_t> zipOf(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    EXPECT_TRUE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, content] : files) {
        EXPECT_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_BEST_SPEED));
    }
    void* data = nullptr;
    std::size_t size = 0;
    EXPECT_TRUE(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> archive(bytes, bytes + size);
    mz_zip_writer_end(&zip);
    return archive;
}

} // namespace

TEST(Setup, FindsWhereAProgramEnds) {
    const auto end = setup::programEnd(smallProgram());
    ASSERT_TRUE(end.ok()) << end.error().message;
    EXPECT_EQ(*end, 0x1000u);
}

TEST(Setup, TakesNothingForAProgramThatIsNone) {
    EXPECT_FALSE(setup::programEnd(std::vector<std::uint8_t>{}).ok());
    EXPECT_FALSE(setup::programEnd(std::vector<std::uint8_t>(4096, 0)).ok());

    std::vector<std::uint8_t> noMark = smallProgram();
    noMark[0x80] = 'X';
    EXPECT_FALSE(setup::programEnd(noMark).ok());

    // The place of "PE" points behind the end of what was read.
    std::vector<std::uint8_t> farAway = smallProgram();
    put32(farAway, 0x3C, 0x7FFFFFF0);
    EXPECT_FALSE(setup::programEnd(farAway).ok());

    // More sections announced than the headers hold.
    std::vector<std::uint8_t> tooMany = smallProgram();
    tooMany[0x80 + 6] = 0xFF;
    tooMany[0x80 + 7] = 0xFF;
    EXPECT_FALSE(setup::programEnd(tooMany).ok());
}

TEST(Setup, CarriesAPackageThatUnpacksToTheSameFiles) {
    const auto folder = freshFolder("setup_round_trip");
    save(folder / "program.exe", smallProgram());
    const std::vector<std::uint8_t> package = zipOf({{"VeloPlay.exe", "the program"}, {"SDL3.dll", "a library"}});

    const auto written = setup::writeSetup(folder / "program.exe", package, folder / "VeloPlay-Setup.exe");
    ASSERT_TRUE(written.ok()) << written.error().message;
    EXPECT_EQ(std::filesystem::file_size(folder / "VeloPlay-Setup.exe"), 0x1000u + 16 + package.size());

    const auto found = setup::readPayload(folder / "VeloPlay-Setup.exe");
    ASSERT_TRUE(found.ok()) << found.error().message;
    EXPECT_EQ(*found, package);

    const auto unpacked = update::extractZip(*found, folder / "unpacked");
    ASSERT_TRUE(unpacked.ok()) << unpacked.error().message;
    std::ifstream program(folder / "unpacked" / "VeloPlay.exe", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(program), {}), "the program");
}

TEST(Setup, FindsThePackageWhenSomethingWasAppendedLater) {
    // A signature is appended at the very end of a file. The package is found from the front.
    const auto folder = freshFolder("setup_signed");
    save(folder / "program.exe", smallProgram());
    const std::vector<std::uint8_t> package = zipOf({{"VeloPlay.exe", "the program"}});
    ASSERT_TRUE(setup::writeSetup(folder / "program.exe", package, folder / "setup.exe").ok());

    std::vector<std::uint8_t> withSignature = load(folder / "setup.exe");
    withSignature.insert(withSignature.end(), 5000, 0x30);
    save(folder / "setup.exe", withSignature);

    const auto found = setup::readPayload(folder / "setup.exe");
    ASSERT_TRUE(found.ok()) << found.error().message;
    EXPECT_EQ(*found, package);
}

TEST(Setup, SaysWhenAFileCarriesNoPackageOrOnlyPartOfIt) {
    const auto folder = freshFolder("setup_broken");
    save(folder / "program.exe", smallProgram());
    EXPECT_FALSE(setup::readPayload(folder / "program.exe").ok());
    EXPECT_FALSE(setup::readPayload(folder / "missing.exe").ok());

    const std::vector<std::uint8_t> package = zipOf({{"VeloPlay.exe", std::string(4000, 'x')}});
    ASSERT_TRUE(setup::writeSetup(folder / "program.exe", package, folder / "setup.exe").ok());
    // A download that broke off.
    std::vector<std::uint8_t> cut = load(folder / "setup.exe");
    cut.resize(cut.size() - 10);
    save(folder / "cut.exe", cut);
    EXPECT_FALSE(setup::readPayload(folder / "cut.exe").ok());

    // Something else sits where the package should be.
    std::vector<std::uint8_t> other = smallProgram();
    other.insert(other.end(), 64, 0x41);
    save(folder / "other.exe", other);
    EXPECT_FALSE(setup::readPayload(folder / "other.exe").ok());
}

TEST(Setup, DoesNotAttachAPackageWhereItWouldNotBeFound) {
    const auto folder = freshFolder("setup_refuse");
    const std::vector<std::uint8_t> package = zipOf({{"VeloPlay.exe", "the program"}});

    // A program with symbols behind its sections, or a setup file that has a package already.
    std::vector<std::uint8_t> withSymbols = smallProgram();
    withSymbols.insert(withSymbols.end(), 100, 0x55);
    save(folder / "symbols.exe", withSymbols);
    EXPECT_FALSE(setup::writeSetup(folder / "symbols.exe", package, folder / "out.exe").ok());

    save(folder / "program.exe", smallProgram());
    EXPECT_FALSE(setup::writeSetup(folder / "program.exe", {}, folder / "out.exe").ok());
    EXPECT_FALSE(setup::writeSetup(folder / "not-there.exe", package, folder / "out.exe").ok());

    save(folder / "text.exe", std::vector<std::uint8_t>(200, 'a'));
    EXPECT_FALSE(setup::writeSetup(folder / "text.exe", package, folder / "out.exe").ok());
}
