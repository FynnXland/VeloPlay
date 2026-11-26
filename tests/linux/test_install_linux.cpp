// Installing on Linux, with every folder below a temporary one: no test
// touches the real ~/.local/share or looks at the real /proc.
#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "linux/InstallerLinux.hpp"

using namespace veloplay;
namespace fs = std::filesystem;

namespace {

fs::path freshInstallFolder(const char* name) {
    const fs::path folder = fs::temp_directory_path() / "veloplay_tests" / name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

void put(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string contentOf(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// An unpacked package in Downloads, and roots for this user below `base`.
struct Package {
    fs::path base;
    fs::path downloads;
    linuxos::InstallRoots roots;

    explicit Package(const char* name) : base(freshInstallFolder(name)) {
        downloads = base / "Downloads";
        put(downloads / "VeloPlay", "program");
        fs::permissions(downloads / "VeloPlay", fs::perms::owner_all);
        put(downloads / "veloplay-cli", "tool");
        put(downloads / "veloplay.png", "picture");
        put(downloads / "README.md", "text");
        put(downloads / "holiday.jpg", "somebody's picture");
        roots.dataHome = base / "home" / ".local" / "share";
        roots.proc = base / "proc";
        fs::create_directories(roots.proc);
    }
};

} // namespace

TEST(LinuxInstall, PutsTheProgramTheMenuEntryAndTheIconInTheUsersFolders) {
    Package p("linux_install_all");
    const auto installer = linuxos::createInstaller(p.downloads / "VeloPlay", p.roots);
    EXPECT_FALSE(installer->installed());
    EXPECT_EQ(installer->target(), p.roots.dataHome / "VeloPlay");
    const auto installed = installer->install();
    ASSERT_TRUE(installed) << installed.error().message;
    EXPECT_EQ(*installed, p.roots.dataHome / "VeloPlay" / "VeloPlay");
    EXPECT_EQ(contentOf(*installed), "program");
    EXPECT_NE(fs::status(*installed).permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_FALSE(fs::exists(p.roots.target() / "holiday.jpg"));
    EXPECT_EQ(contentOf(p.roots.iconFile()), "picture");
    EXPECT_EQ(p.roots.iconFile(), p.roots.dataHome / "icons/hicolor/256x256/apps/veloplay.png");

    const std::string entry = contentOf(p.roots.desktopFile());
    EXPECT_EQ(p.roots.desktopFile().filename(), "veloplay.desktop");
    EXPECT_NE(entry.find("[Desktop Entry]\n"), std::string::npos);
    EXPECT_NE(entry.find("\nName=VeloPlay\n"), std::string::npos);
    EXPECT_NE(entry.find("\nExec=\"" + installed->string() + "\"\n"), std::string::npos) << entry;
    EXPECT_NE(entry.find("\nIcon=veloplay\n"), std::string::npos);
    EXPECT_NE(entry.find("\nCategories=Game;\n"), std::string::npos);
    EXPECT_NE(entry.find("\nStartupWMClass=veloplay\n"), std::string::npos);
    EXPECT_NE(entry.find("\nActions=uninstall;\n"), std::string::npos);
    EXPECT_NE(entry.find("\n[Desktop Action uninstall]\nName=Remove VeloPlay\nName[de]=VeloPlay entfernen\nExec=\"" +
                         installed->string() + "\" --uninstall\n"),
              std::string::npos)
        << entry;

    // The installed program knows it is the installed one; installing it again changes nothing.
    const auto again = linuxos::createInstaller(*installed, p.roots);
    EXPECT_TRUE(again->installed());
    EXPECT_EQ(again->install().value(), *installed);
}

// Exec: quotes around the path, a backslash before " ` $ \, then every backslash doubled, % doubled.
TEST(LinuxInstall, QuotesTheProgramForTheExecKey) {
    EXPECT_EQ(linuxos::desktopExecArgument("/home/jörg/.local/share/VeloPlay/VeloPlay"),
              "\"/home/jörg/.local/share/VeloPlay/VeloPlay\"");
    EXPECT_EQ(linuxos::desktopExecArgument("/a b/c"), "\"/a b/c\"");
    EXPECT_EQ(linuxos::desktopExecArgument("/x\"y"), "\"/x\\\\\"y\"");
    EXPECT_EQ(linuxos::desktopExecArgument("/x$HOME`id`"), "\"/x\\\\$HOME\\\\`id\\\\`\"");
    EXPECT_EQ(linuxos::desktopExecArgument("/back\\slash"), "\"/back\\\\\\\\slash\"");
    EXPECT_EQ(linuxos::desktopExecArgument("/100%/x"), "\"/100%%/x\"");
}

TEST(LinuxInstall, PutsBackTheEntryAfterAnUpdateOnlyForTheInstalledProgram) {
    Package p("linux_install_refresh");
    linuxos::refreshInstallation(p.downloads / "VeloPlay", p.roots);
    EXPECT_FALSE(fs::exists(p.roots.desktopFile())); // not installed: nothing written
    const fs::path program = *linuxos::createInstaller(p.downloads / "VeloPlay", p.roots)->install();
    fs::remove(p.roots.desktopFile());
    put(p.roots.target() / "veloplay.png", "new picture"); // an update brought a new icon
    linuxos::refreshInstallation(program, p.roots);
    EXPECT_TRUE(fs::exists(p.roots.desktopFile()));
    EXPECT_EQ(contentOf(p.roots.iconFile()), "new picture");
}

TEST(LinuxInstall, RemovesEverythingButNotWhileItRunsElsewhere) {
    Package p("linux_install_remove");
    const fs::path program = *linuxos::createInstaller(p.downloads / "VeloPlay", p.roots)->install();
    // Only the installed program removes itself.
    EXPECT_EQ(linuxos::uninstall(p.downloads / "VeloPlay", true, {}, p.roots), 1);

    // Another VeloPlay from that folder: its /proc entry points at the program, even when an update renamed it.
    fs::create_directories(p.roots.proc / "4242");
    fs::create_symlink(p.roots.target() / "VeloPlay.old (deleted)", p.roots.proc / "4242" / "exe");
    fs::create_directories(p.roots.proc / "4343");
    fs::create_symlink("/usr/bin/bash", p.roots.proc / "4343" / "exe");
    fs::create_directories(p.roots.proc / "self");
    std::vector<linuxos::UninstallOutcome> told;
    const auto tell = [&](linuxos::UninstallOutcome outcome) { told.push_back(outcome); };
    EXPECT_EQ(linuxos::uninstall(program, false, tell, p.roots), 2);
    ASSERT_EQ(told.size(), 1u);
    EXPECT_EQ(told[0], linuxos::UninstallOutcome::StillRunning);
    EXPECT_TRUE(fs::exists(program));
    EXPECT_EQ(linuxos::uninstall(program, true, tell, p.roots), 2);
    EXPECT_EQ(told.size(), 1u); // quiet: nothing told

    // This process itself does not count.
    fs::remove_all(p.roots.proc / "4242");
    fs::create_directories(p.roots.proc / std::to_string(::getpid()));
    fs::create_symlink(program, p.roots.proc / std::to_string(::getpid()) / "exe");
    EXPECT_EQ(linuxos::uninstall(program, false, tell, p.roots), 0);
    ASSERT_EQ(told.size(), 2u);
    EXPECT_EQ(told[1], linuxos::UninstallOutcome::Removed);
    EXPECT_FALSE(fs::exists(p.roots.target()));
    EXPECT_FALSE(fs::exists(p.roots.desktopFile()));
    EXPECT_FALSE(fs::exists(p.roots.iconFile()));
    EXPECT_TRUE(fs::exists(p.downloads / "VeloPlay")); // what was unpacked stays
}

TEST(LinuxInstall, LeavesAFolderAloneThatInstallingDidNotMake) {
    Package p("linux_install_foreign");
    // Somebody's own folder of that name, without the program in it.
    put(p.roots.target() / "diary.txt", "private");
    EXPECT_EQ(linuxos::uninstall(p.roots.target() / "VeloPlay", true, {}, p.roots), 1);
    EXPECT_TRUE(fs::exists(p.roots.target() / "diary.txt"));
}

TEST(LinuxInstall, FindsTheUsersDataFolder) {
    const linuxos::InstallRoots roots = linuxos::installRoots();
    EXPECT_FALSE(roots.dataHome.empty());
    EXPECT_EQ(roots.target().filename(), "VeloPlay");
}
