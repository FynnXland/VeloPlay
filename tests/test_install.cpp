#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "app/Installer.hpp"

using namespace veloplay;

namespace {

std::filesystem::path freshFolder(const char* name) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "veloplay_tests" / name;
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

void write(const std::filesystem::path& file, const char* text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

} // namespace

// The archive has its files at the top level. Unpacked with "extract here",
// VeloPlay lies in Downloads next to everything else there.
TEST(Install, CopiesOnlyThePackageAndNothingThatLiesNextToIt) {
    const std::filesystem::path from = freshFolder("install_from");
    const std::filesystem::path to = freshFolder("install_to") / "VeloPlay";
    write(from / "VeloPlay.exe", "program");
    write(from / "veloplay-cli.exe", "tool");
    write(from / "avcodec-63.dll", "library");
    write(from / "WebView2Loader.DLL", "library");
    for (const char* document : {"README.md", "README.de.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY.md"}) {
        write(from / document, "text");
    }
    // What else is in Downloads.
    write(from / "holiday.jpg", "a picture");
    write(from / "notes.md", "somebody's notes");
    write(from / "Projects" / "tool.exe", "another program");
    write(from / "Projects" / "letter.docx", "a letter");
    write(from / "VeloPlay.exe.old", "the version before");
    // Something from an installation before is replaced, not kept.
    write(to / "VeloPlay.exe", "older program");

    const Result<int> copied = install::copyProgram(from, to);
    ASSERT_TRUE(copied.ok()) << copied.error().message;
    EXPECT_EQ(copied.value(), 9);
    EXPECT_EQ(read(to / "VeloPlay.exe"), "program");
    EXPECT_EQ(read(to / "avcodec-63.dll"), "library");
    EXPECT_TRUE(std::filesystem::exists(to / "WebView2Loader.DLL"));
    EXPECT_TRUE(std::filesystem::exists(to / "THIRD_PARTY.md"));
    for (const char* stranger : {"holiday.jpg", "notes.md", "Projects", "VeloPlay.exe.old"}) {
        EXPECT_FALSE(std::filesystem::exists(to / stranger)) << stranger;
    }
    // The folder it came from is untouched.
    EXPECT_TRUE(std::filesystem::exists(from / "holiday.jpg"));
    EXPECT_TRUE(std::filesystem::exists(from / "Projects" / "letter.docx"));
}

TEST(Install, RefusesWhatCannotWork) {
    const std::filesystem::path from = freshFolder("install_refuse");
    // Nothing there to install, or something without the program.
    EXPECT_FALSE(install::copyProgram(from, freshFolder("install_refuse_to")).ok());
    write(from / "SDL3.dll", "library");
    EXPECT_FALSE(install::copyProgram(from, freshFolder("install_refuse_to")).ok());
    EXPECT_FALSE(install::copyProgram(from / "missing", freshFolder("install_refuse_to")).ok());
    // Into itself: the copy would feed on its own output.
    write(from / "VeloPlay.exe", "program");
    EXPECT_FALSE(install::copyProgram(from, from / "inner").ok());
}

TEST(Install, TellsWhetherAFileLiesInAFolder) {
    const std::filesystem::path folder = freshFolder("install_inside");
    write(folder / "VeloPlay" / "VeloPlay.exe", "program");
    EXPECT_TRUE(install::isInside(folder / "VeloPlay" / "VeloPlay.exe", folder / "VeloPlay"));
    EXPECT_TRUE(install::isInside(folder / "VeloPlay" / "VeloPlay.exe", folder));
    EXPECT_FALSE(install::isInside(folder / "VeloPlay.exe", folder / "VeloPlay"));
    // A folder whose name merely starts the same is another folder.
    EXPECT_FALSE(install::isInside(folder / "VeloPlay2" / "VeloPlay.exe", folder / "VeloPlay"));
    EXPECT_FALSE(install::isInside(folder / "VeloPlay.exe", std::filesystem::path{}));
}

TEST(Install, RemovesOnlyAFolderThatInstallingMade) {
    const std::filesystem::path folder = freshFolder("install_remove");
    write(folder / "VeloPlay" / "VeloPlay.exe", "program");
    EXPECT_TRUE(install::isRemovable(folder / "VeloPlay"));
    // Not a folder of another name, not one without the program in it.
    write(folder / "Documents" / "VeloPlay.exe", "program");
    EXPECT_FALSE(install::isRemovable(folder / "Documents"));
    std::filesystem::create_directories(folder / "empty" / "VeloPlay");
    EXPECT_FALSE(install::isRemovable(folder / "empty" / "VeloPlay"));
    EXPECT_FALSE(install::isRemovable(std::filesystem::path{}));
}
