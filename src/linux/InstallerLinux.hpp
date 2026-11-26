#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "app/Installer.hpp"

namespace veloplay::linuxos {

/// The name the desktop knows VeloPlay by: the desktop file is veloplay.desktop,
/// and the window must carry the same name (SDL_HINT_APP_ID, which SDL uses for
/// the Wayland app id and the X11 window class), or the dock shows a second,
/// nameless icon next to the entry.
inline constexpr const char* kDesktopAppId = "veloplay";

/// Where installing puts things, all below the user's own data folder.
struct InstallRoots {
    std::filesystem::path dataHome;       // $XDG_DATA_HOME, otherwise ~/.local/share
    std::filesystem::path proc = "/proc"; // to see whether VeloPlay runs elsewhere

    [[nodiscard]] std::filesystem::path target() const { return dataHome / "VeloPlay"; }
    [[nodiscard]] std::filesystem::path desktopFile() const { return dataHome / "applications" / "veloplay.desktop"; }
    [[nodiscard]] std::filesystem::path iconFile() const {
        return dataHome / "icons" / "hicolor" / "256x256" / "apps" / "veloplay.png";
    }
};

/// The roots of this user, from the environment.
[[nodiscard]] InstallRoots installRoots();

/// Installs per user, as on Windows without administrator rights: the package
/// into $XDG_DATA_HOME/VeloPlay, an entry for the desktop's application menu,
/// and the icon (veloplay.png from the package, 256 x 256) into the hicolor theme.
/// `program` is the file that runs (/proc/self/exe).
std::unique_ptr<IInstaller> createInstaller(const std::filesystem::path& program);
std::unique_ptr<IInstaller> createInstaller(const std::filesystem::path& program, InstallRoots roots);

/// Writes the menu entry and the icon again if the running program is the
/// installed one: an update may bring a new icon, and a deleted entry comes back.
void refreshInstallation(const std::filesystem::path& program);
void refreshInstallation(const std::filesystem::path& program, const InstallRoots& roots);

/// What uninstall() tells the user unless it is quiet: the program runs elsewhere, or it is gone.
enum class UninstallOutcome { StillRunning, Removed };

/// Removes the menu entry, the icon and the installed folder. Only the
/// installed program removes itself (1 otherwise), only a folder that
/// installing made (install::isRemovable), and not while another VeloPlay
/// from that folder runs (2): found by reading /proc/<pid>/exe of every
/// process, which needs no cooperation of the other one and also sees a
/// program an update has renamed to VeloPlay.old. 0 when it is done. Linux
/// lets a running program delete its own file, so nothing waits for the end.
/// `tell` is called with the outcome unless `quiet`, for a message box.
int uninstall(const std::filesystem::path& program, bool quiet, const std::function<void(UninstallOutcome)>& tell = {});
int uninstall(const std::filesystem::path& program, bool quiet, const std::function<void(UninstallOutcome)>& tell,
              const InstallRoots& roots);

/// The menu entry for `program` (Desktop Entry Specification 1.5).
[[nodiscard]] std::string desktopEntry(const std::filesystem::path& program);

/// `argument` for the Exec key: in double quotes with ", `, $ and \ escaped,
/// then escaped again as a string value (each \ doubled), and % doubled.
[[nodiscard]] std::string desktopExecArgument(const std::string& argument);

/// Whether a process other than `self` runs a program from `folder`.
[[nodiscard]] bool runsFrom(const std::filesystem::path& folder, const std::filesystem::path& proc, int self);

} // namespace veloplay::linuxos
