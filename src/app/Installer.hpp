#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "util/Result.hpp"

namespace veloplay {

/// Makes VeloPlay a program that is installed on this PC instead of a folder
/// somebody unpacked: its files in a fixed place of the user's own, an entry
/// in the start menu, and one in Windows' list of installed apps, from where
/// it can be removed. All of it per user; nothing needs administrator rights.
/// The updater works there as anywhere: it replaces files next to the program.
class IInstaller {
public:
    virtual ~IInstaller() = default;
    /// Whether the running program is the installed one.
    [[nodiscard]] virtual bool installed() const = 0;
    /// The folder the program is installed into.
    [[nodiscard]] virtual std::filesystem::path target() const = 0;
    /// Copies the program, makes the two entries and gives back the installed program to start.
    virtual Result<std::filesystem::path> install() = 0;
};

namespace install {

/// The name of the program file, in the folder it runs from and in the one it is installed into.
inline constexpr const char* kProgramFile = "VeloPlay.exe";

/// Whether a file of that name belongs to the package VeloPlay comes in:
/// programs, libraries and the five documents. Nothing else is copied.
/// The archive has everything at its top level, so "extract here" can put
/// VeloPlay into Downloads or onto the desktop, next to anything else there.
[[nodiscard]] bool belongsToPackage(const std::filesystem::path& file);

/// Copies the files of the package that lie directly in `from` into `to`,
/// over what is there. No folders, nothing else. Gives back the number of
/// files copied.
Result<int> copyProgram(const std::filesystem::path& from, const std::filesystem::path& to);

/// A path as UTF-8 text, for the log and the interface. path::string() uses
/// the ANSI code page on Windows and garbles or throws for a name like Jörg.
[[nodiscard]] std::string utf8(const std::filesystem::path& path);

/// Whether `file` lies in `folder` or below it.
[[nodiscard]] bool isInside(const std::filesystem::path& file, const std::filesystem::path& folder);

/// A folder may be removed on uninstalling only if it is what installing
/// made: it is called VeloPlay and holds the program. Anything else stays.
[[nodiscard]] bool isRemovable(const std::filesystem::path& folder);

} // namespace install

#ifdef _WIN32
/// `program` is the file that is running.
std::unique_ptr<IInstaller> createInstaller(const std::filesystem::path& program);

/// Writes the installed version into Windows' list of apps again; an update changes it.
void refreshInstallation(const std::filesystem::path& program);

/// Removes the two entries and the installed folder. Gives back the exit
/// code of the program. With `quiet` nothing is shown. `runningText` and
/// `doneText` are what the user is told otherwise.
int uninstall(const std::filesystem::path& program, bool quiet, const wchar_t* windowClass, const std::wstring& runningText,
              const std::wstring& doneText);
#endif

} // namespace veloplay
