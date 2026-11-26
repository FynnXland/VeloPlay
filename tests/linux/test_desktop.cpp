#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "linux/DesktopLinux.hpp"

using namespace veloplay;
using linuxos::UserDirectory;
namespace fs = std::filesystem;

namespace {

fs::path freshDesktopFolder(const char* name) {
    const fs::path folder = fs::temp_directory_path() / "veloplay_tests" / name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

// As xdg-user-dirs-update writes it on a German desktop.
constexpr char kGermanDirs[] = R"(# This file is written by xdg-user-dirs-update
# If you want to change or add directories, just edit the line you're
# interested in. All local changes will be retained on the next run.
XDG_DESKTOP_DIR="$HOME/Schreibtisch"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_PICTURES_DIR="$HOME/Bilder"
XDG_VIDEOS_DIR="$HOME/Videos"
)";

} // namespace

TEST(LinuxDesktop, ReadsTheFoldersTheDesktopNames) {
    const fs::path home = "/home/jörg";
    EXPECT_EQ(linuxos::parseUserDirs(kGermanDirs, "XDG_PICTURES_DIR", home), fs::path("/home/jörg/Bilder"));
    EXPECT_EQ(linuxos::parseUserDirs(kGermanDirs, "XDG_VIDEOS_DIR", home), fs::path("/home/jörg/Videos"));
    EXPECT_FALSE(linuxos::parseUserDirs(kGermanDirs, "XDG_MUSIC_DIR", home));
    // Absolute paths, ${HOME}, quotes inside, spaces around and a trailing slash.
    EXPECT_EQ(linuxos::parseUserDirs("XDG_PICTURES_DIR=\"/data/Fotos/\"", "XDG_PICTURES_DIR", home),
              fs::path("/data/Fotos"));
    EXPECT_EQ(linuxos::parseUserDirs("  XDG_VIDEOS_DIR = \"${HOME}/Meine \\\"Filme\\\"\"\n", "XDG_VIDEOS_DIR", home),
              fs::path("/home/jörg/Meine \"Filme\""));
    // A later line wins, as in the shell that reads the file.
    EXPECT_EQ(
        linuxos::parseUserDirs("XDG_PICTURES_DIR=\"$HOME/A\"\nXDG_PICTURES_DIR=\"$HOME/B\"", "XDG_PICTURES_DIR", home),
        fs::path("/home/jörg/B"));
}

TEST(LinuxDesktop, IgnoresWhatIsSwitchedOffOrNoPath) {
    const fs::path home = "/home/jörg";
    for (const char* text : {"XDG_PICTURES_DIR=\"$HOME/\"", "XDG_PICTURES_DIR=\"$HOME\"", "XDG_PICTURES_DIR=\"Bilder\"",
                             "XDG_PICTURES_DIR=\"$HOMEWORK/x\"", "XDG_PICTURES_DIR=", "# XDG_PICTURES_DIR=\"/x\"",
                             "XDG_PICTURES_DIRS=\"/x\"", ""}) {
        EXPECT_FALSE(linuxos::parseUserDirs(text, "XDG_PICTURES_DIR", home)) << text;
    }
}

TEST(LinuxDesktop, FallsBackToPicturesAndVideosInTheHomeFolder) {
    const fs::path home = freshDesktopFolder("desktop_home");
    const fs::path config = home / ".config";
    EXPECT_EQ(linuxos::userDirectory(UserDirectory::Pictures, home, config), home / "Pictures");
    EXPECT_EQ(linuxos::userDirectory(UserDirectory::Videos, home, config), home / "Videos");
    fs::create_directories(config);
    std::ofstream(config / "user-dirs.dirs") << kGermanDirs;
    EXPECT_EQ(linuxos::userDirectory(UserDirectory::Pictures, home, config), home / "Bilder");
    EXPECT_EQ(linuxos::userDirectory(UserDirectory::Videos, home, config), home / "Videos");
    EXPECT_FALSE(linuxos::userDirectory(UserDirectory::Pictures).empty());
}

TEST(LinuxDesktop, NeverHandsXdgOpenAnOption) {
    EXPECT_FALSE(linuxos::openWithDesktop(""));
    EXPECT_FALSE(linuxos::openWithDesktop("--help"));
    EXPECT_FALSE(linuxos::openWithDesktop("-x"));
}

// A stand-in xdg-open on PATH writes down what it got: one argument, word for word, no shell in between.
TEST(LinuxDesktop, StartsXdgOpenWithTheTextAsItIs) {
    const fs::path bin = freshDesktopFolder("desktop_bin");
    const fs::path out = bin / "got.txt";
    {
        std::ofstream script(bin / "xdg-open");
        script << "#!/bin/sh\nprintf '%s|%s' \"$#\" \"$1\" > '" << out.string() << ".part'\nmv '" << out.string()
               << ".part' '" << out.string() << "'\n";
    }
    fs::permissions(bin / "xdg-open", fs::perms::owner_all);
    const std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    setenv("PATH", (bin.string() + ":" + oldPath).c_str(), 1);
    const std::string address = "https://example.com/a b?x=$(touch /tmp/veloplay_pwned)&y=`id`;z";
    const bool started = linuxos::openWithDesktop(address);
    setenv("PATH", oldPath.c_str(), 1);
    ASSERT_TRUE(started);
    for (int i = 0; i < 200 && !fs::exists(out); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::ifstream file(out);
    std::stringstream got;
    got << file.rdbuf();
    EXPECT_EQ(got.str(), "1|" + address);
    EXPECT_FALSE(fs::exists("/tmp/veloplay_pwned"));
}
