#include "app/Installer.hpp"

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <spdlog/spdlog.h>

namespace veloplay {

namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VeloPlay";

std::filesystem::path knownFolder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) path = raw;
    if (raw) CoTaskMemFree(raw);
    return path;
}

/// Where programs go that a user installs for themselves: %LOCALAPPDATA%\Programs.
std::filesystem::path installFolder() {
    const std::filesystem::path programs = knownFolder(FOLDERID_UserProgramFiles);
    return programs.empty() ? std::filesystem::path{} : programs / L"VeloPlay";
}

std::filesystem::path startMenuEntry() {
    const std::filesystem::path menu = knownFolder(FOLDERID_Programs);
    return menu.empty() ? std::filesystem::path{} : menu / L"VeloPlay.lnk";
}

bool writeShortcut(const std::filesystem::path& entry, const std::filesystem::path& program) {
    // Works with or without a COM apartment on this thread already.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool written = false;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   reinterpret_cast<void**>(&link))) &&
        link) {
        link->SetPath(program.c_str());
        link->SetWorkingDirectory(program.parent_path().c_str());
        link->SetDescription(L"Remote Play for the PS5");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) && file) {
            written = SUCCEEDED(file->Save(entry.c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return written;
}

void setText(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void setNumber(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

/// The entry in Windows' list of installed apps (Settings > Apps), for this user.
bool writeAppEntry(const std::filesystem::path& program) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring file = program.wstring();
    const std::string version = VELOPLAY_VERSION;
    std::uintmax_t bytes = 0;
    std::error_code error;
    for (auto entry = std::filesystem::recursive_directory_iterator(program.parent_path(), error);
         !error && entry != std::filesystem::recursive_directory_iterator(); entry.increment(error)) {
        if (entry->is_regular_file(error)) bytes += entry->file_size(error);
    }
    setText(key, L"DisplayName", L"VeloPlay");
    setText(key, L"DisplayVersion", std::wstring(version.begin(), version.end()));
    setText(key, L"Publisher", L"VeloPlay (open source)");
    setText(key, L"InstallLocation", program.parent_path().wstring());
    setText(key, L"DisplayIcon", file);
    setText(key, L"UninstallString", L"\"" + file + L"\" --uninstall");
    setText(key, L"QuietUninstallString", L"\"" + file + L"\" --uninstall --quiet");
    setText(key, L"URLInfoAbout", L"https://github.com/FynnXland/VeloPlay");
    setNumber(key, L"NoModify", 1);
    setNumber(key, L"NoRepair", 1);
    setNumber(key, L"EstimatedSize", static_cast<DWORD>(bytes / 1024));
    RegCloseKey(key);
    return true;
}

class WindowsInstaller final : public IInstaller {
public:
    explicit WindowsInstaller(std::filesystem::path program)
        : program_(std::move(program)), target_(installFolder()),
          installed_(!target_.empty() && install::isInside(program_, target_)) {}

    // Asked every frame; where the program runs does not change while it runs.
    [[nodiscard]] bool installed() const override { return installed_; }
    [[nodiscard]] std::filesystem::path target() const override { return target_; }

    Result<std::filesystem::path> install() override {
        if (target_.empty()) return Error{ErrorCode::Io, "Windows did not name a folder for programs of this user"};
        const std::filesystem::path installedProgram = target_ / install::kProgramFile;
        if (installed()) return installedProgram;
        const Result<int> copied = install::copyProgram(program_.parent_path(), target_);
        if (!copied) return copied.error();
        // Without these two the program still works from its new place; say so and go on.
        const std::filesystem::path entry = startMenuEntry();
        if (entry.empty() || !writeShortcut(entry, installedProgram)) spdlog::warn("Install: no entry in the start menu");
        if (!writeAppEntry(installedProgram)) spdlog::warn("Install: no entry in the list of installed apps");
        spdlog::info("Install: {} files copied to {}", copied.value(), install::utf8(target_));
        return installedProgram;
    }

private:
    std::filesystem::path program_;
    std::filesystem::path target_;
    bool installed_ = false;
};

} // namespace

std::unique_ptr<IInstaller> createInstaller(const std::filesystem::path& program) {
    return std::make_unique<WindowsInstaller>(program);
}

void refreshInstallation(const std::filesystem::path& program) {
    const std::filesystem::path target = installFolder();
    if (target.empty() || !install::isInside(program, target)) return;
    writeAppEntry(program);
}

int uninstall(const std::filesystem::path& program, bool quiet, const wchar_t* windowClass, const std::wstring& runningText,
              const std::wstring& doneText) {
    const std::filesystem::path target = installFolder();
    // Only the installed program removes itself, and only the folder that installing made.
    if (target.empty() || !install::isInside(program, target) || !install::isRemovable(target)) return 1;
    // Its files cannot go while a window of it is open.
    if (FindWindowW(windowClass, nullptr)) {
        if (!quiet) MessageBoxW(nullptr, runningText.c_str(), L"VeloPlay", MB_OK | MB_ICONWARNING);
        return 2;
    }
    const std::filesystem::path entry = startMenuEntry();
    if (!entry.empty()) DeleteFileW(entry.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    if (!quiet) MessageBoxW(nullptr, doneText.c_str(), L"VeloPlay", MB_OK | MB_ICONINFORMATION);

    // A program cannot delete the file it runs from. A command prompt does it
    // a moment after this one has ended, from a folder that is not the one to go.
    std::wstring command = L"cmd.exe /d /c \"ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + target.wstring() + L"\"\"";
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH, temp);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temp, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return 0;
}

} // namespace veloplay
