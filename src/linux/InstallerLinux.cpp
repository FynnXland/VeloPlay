#include "linux/InstallerLinux.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include <spdlog/spdlog.h>

#include "linux/DesktopLinux.hpp"

namespace veloplay::linuxos {

namespace {

namespace fs = std::filesystem;
using update::kLinuxPackage;

/// Writes beside and renames, so the desktop, which watches the folder, never reads half a file.
bool writeAtomically(const fs::path& file, const std::string& text) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error) return false;
    const fs::path part = fs::path(file).concat(".part");
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out.flush()) return false;
    }
    fs::rename(part, file, error);
    if (error) fs::remove(part, error);
    return !error;
}

bool copyIcon(const fs::path& folder, const InstallRoots& roots) {
    const fs::path icon = folder / install::kLinuxIconFile;
    std::error_code error;
    if (!fs::is_regular_file(icon, error)) return false;
    fs::create_directories(roots.iconFile().parent_path(), error);
    fs::copy_file(icon, roots.iconFile(), fs::copy_options::overwrite_existing, error);
    return !error;
}

/// The two entries for the desktop. Without them the program still works from its new place; say so and go on.
void writeEntries(const fs::path& installedProgram, const InstallRoots& roots) {
    if (!writeAtomically(roots.desktopFile(), desktopEntry(installedProgram))) {
        spdlog::warn("Install: no entry in the application menu ({})", install::utf8(roots.desktopFile()));
    }
    if (!copyIcon(installedProgram.parent_path(), roots)) spdlog::warn("Install: no icon for the application menu");
}

class LinuxInstaller final : public IInstaller {
public:
    LinuxInstaller(fs::path program, InstallRoots roots)
        : program_(std::move(program)), roots_(std::move(roots)),
          installed_(!roots_.dataHome.empty() && install::isInside(program_, roots_.target())) {}

    // Asked every frame; where the program runs does not change while it runs.
    [[nodiscard]] bool installed() const override { return installed_; }
    [[nodiscard]] fs::path target() const override { return roots_.dataHome.empty() ? fs::path{} : roots_.target(); }

    Result<fs::path> install() override {
        if (roots_.dataHome.empty()) return Error{ErrorCode::Io, "there is no folder for this user's data ($HOME)"};
        const fs::path installedProgram = roots_.target() / kLinuxPackage.programFile;
        if (installed()) return installedProgram;
        const std::string path = install::utf8(installedProgram);
        if (path.find_first_of("\n\r") != std::string::npos) {
            return Error{ErrorCode::InvalidArgument, "the folder to install into has a line break in its name"};
        }
        const Result<int> copied = install::copyProgram(program_.parent_path(), roots_.target(), kLinuxPackage);
        if (!copied) return copied.error();
        writeEntries(installedProgram, roots_);
        spdlog::info("Install: {} files copied to {}", copied.value(), install::utf8(roots_.target()));
        return installedProgram;
    }

private:
    fs::path program_;
    InstallRoots roots_;
    bool installed_ = false;
};

} // namespace

InstallRoots installRoots() {
    InstallRoots roots;
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data == '/') {
        roots.dataHome = data;
    } else {
        roots.dataHome = homeDirectory() / ".local" / "share";
    }
    return roots;
}

std::string desktopExecArgument(const std::string& argument) {
    // Inside the quotes of an Exec argument these four take a backslash.
    std::string quoted = "\"";
    for (const char c : argument) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    // The value of the key is a string first: each backslash written twice; and % starts a field code.
    std::string value;
    for (const char c : quoted) {
        if (c == '\\')
            value += "\\\\";
        else if (c == '%')
            value += "%%";
        else
            value += c;
    }
    return value;
}

std::string desktopEntry(const fs::path& program) {
    const std::string exec = desktopExecArgument(install::utf8(program));
    // Linux has no list of installed apps to remove programs from; the menu entry offers it instead.
    // The desktop shows the name in the language of the session itself (Name[de]), so it is written here
    // in both languages rather than in the one VeloPlay runs in.
    return std::string("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Version=1.5\n"
                       "Name=VeloPlay\n"
                       "Exec=") +
           exec +
           "\n"
           "Icon=veloplay\n"
           "Terminal=false\n"
           "Categories=Game;\n"
           "StartupWMClass=" +
           kDesktopAppId +
           "\n"
           "Actions=uninstall;\n"
           "\n"
           "[Desktop Action uninstall]\n"
           "Name=Remove VeloPlay\n"
           "Name[de]=VeloPlay entfernen\n"
           "Exec=" +
           exec + " --uninstall\n";
}

bool runsFrom(const fs::path& folder, const fs::path& proc, int self) {
    std::error_code error;
    for (auto entry = fs::directory_iterator(proc, error); !error && entry != fs::directory_iterator();
         entry.increment(error)) {
        const std::string name = entry->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(),
                                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            continue;
        }
        if (std::strtoll(name.c_str(), nullptr, 10) == self) continue;
        std::error_code unreadable; // other users' processes
        std::string exe = fs::read_symlink(entry->path() / "exe", unreadable).string();
        if (unreadable || exe.empty()) continue;
        // The kernel marks a program whose file has been replaced or removed.
        constexpr std::string_view kDeleted = " (deleted)";
        if (exe.ends_with(kDeleted)) exe.erase(exe.size() - kDeleted.size());
        if (install::isInside(exe, folder)) return true;
    }
    return false;
}

std::unique_ptr<IInstaller> createInstaller(const fs::path& program) {
    return createInstaller(program, installRoots());
}

std::unique_ptr<IInstaller> createInstaller(const fs::path& program, InstallRoots roots) {
    return std::make_unique<LinuxInstaller>(program, std::move(roots));
}

void refreshInstallation(const fs::path& program) {
    refreshInstallation(program, installRoots());
}

void refreshInstallation(const fs::path& program, const InstallRoots& roots) {
    if (roots.dataHome.empty() || !install::isInside(program, roots.target())) return;
    writeEntries(roots.target() / kLinuxPackage.programFile, roots);
}

int uninstall(const fs::path& program, bool quiet, const std::function<void(UninstallOutcome)>& tell) {
    return uninstall(program, quiet, tell, installRoots());
}

int uninstall(const fs::path& program, bool quiet, const std::function<void(UninstallOutcome)>& tell,
              const InstallRoots& roots) {
    const fs::path target = roots.target();
    // Only the installed program removes itself, and only the folder that installing made.
    if (roots.dataHome.empty() || !install::isInside(program, target) || !install::isRemovable(target, kLinuxPackage)) {
        return 1;
    }
    if (runsFrom(target, roots.proc, static_cast<int>(::getpid()))) {
        if (!quiet && tell) tell(UninstallOutcome::StillRunning);
        return 2;
    }
    std::error_code error;
    fs::remove(roots.desktopFile(), error);
    fs::remove(roots.iconFile(), error);
    fs::remove_all(target, error);
    if (error) {
        spdlog::warn("Uninstall: {} could not be removed: {}", install::utf8(target), error.message());
        return 1;
    }
    if (!quiet && tell) tell(UninstallOutcome::Removed);
    return 0;
}

} // namespace veloplay::linuxos
