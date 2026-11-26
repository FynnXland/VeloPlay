// VeloPlay-Setup.exe: one file to double-click. It carries the package of a
// release behind its own program (see setup/Payload.hpp), unpacks it to a
// temporary folder and lets the VeloPlay in there install itself, exactly as
// the button in the settings does. So there is one way of installing, not two.
//
//   VeloPlay-Setup.exe                  install, then start VeloPlay
//   VeloPlay-Setup.exe --quiet          install; no window, no question, no start
//   VeloPlay-Setup.exe --check          unpack and look for the program; installs nothing
//   <program> --make <zip> <setup file> attach a package (what the release does)
//
// It stands alone: it runs before anything is unpacked, so it may need nothing
// but Windows itself.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "setup/Payload.hpp"
#include "update/Zip.hpp"

namespace {

namespace fs = std::filesystem;
using namespace veloplay;

constexpr wchar_t kWindowClass[] = L"VeloPlayWindow"; // the class of VeloPlay's own window (Application.cpp)
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VeloPlay";
constexpr wchar_t kProgram[] = L"VeloPlay.exe";
constexpr DWORD kInstallTimeoutMs = 5 * 60 * 1000;

enum ExitCode : int {
    kDone = 0,
    kNoPackage = 2,
    kNotUnpacked = 3,
    kStillRunning = 4,
    kNotInstalled = 5,
    kBadArguments = 6,
    kNotWritten = 7,
};

struct Texts {
    const wchar_t* installing;
    const wchar_t* running;
    const wchar_t* incomplete;
    const wchar_t* failed;
};

constexpr Texts kEnglish{
    L"Installing VeloPlay …",
    L"VeloPlay is running. Close it, then choose Retry.",
    L"This setup file is incomplete. Please download it again.",
    L"VeloPlay could not be installed. Try again. If that does not help, take the ZIP file from the download page.",
};
constexpr Texts kGerman{
    L"VeloPlay wird installiert …",
    L"VeloPlay läuft gerade. Schließe es und wähle dann „Wiederholen“.",
    L"Diese Setup-Datei ist unvollständig. Lade sie bitte noch einmal herunter.",
    L"VeloPlay konnte nicht installiert werden. Versuch es noch einmal. Hilft das nicht, nimm die ZIP-Datei von "
    L"der Download-Seite.",
};

const Texts& texts() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN ? kGerman : kEnglish;
}

fs::path ownFile() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

/// Where the installed program is: the installer writes it down for Windows' list of apps.
fs::path installedProgram() {
    wchar_t folder[1024];
    DWORD bytes = sizeof(folder);
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, folder, &bytes) ==
            ERROR_SUCCESS &&
        folder[0] != L'\0') {
        return fs::path(folder) / kProgram;
    }
    PWSTR raw = nullptr;
    fs::path programs;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &raw)) && raw) programs = raw;
    if (raw) CoTaskMemFree(raw);
    return programs.empty() ? fs::path{} : programs / L"VeloPlay" / kProgram;
}

/// Runs `program --install` without a window and waits for it.
bool installFrom(const fs::path& program) {
    std::wstring command = L"\"" + program.wstring() + L"\" --install";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        program.parent_path().c_str(), &startup, &process)) {
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(process.hProcess, kInstallTimeoutMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

/// Lets the unpacked VeloPlay install itself; the installed program, or nothing if that failed.
fs::path installedProgramAfter(const fs::path& unpackedProgram) {
    if (!installFrom(unpackedProgram)) return {};
    const fs::path installed = installedProgram();
    std::error_code ec;
    return !installed.empty() && fs::exists(installed, ec) ? installed : fs::path{};
}

void removeFolder(const fs::path& folder) {
    // A program that has just ended, or a virus scanner looking at what was unpacked, can hold a file a moment longer.
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::error_code error;
        fs::remove_all(folder, error);
        if (!error && !fs::exists(folder, error)) return;
        Sleep(200);
    }
}

fs::path temporaryFolder() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return {};
    return base / (L"VeloPlay-Setup-" + std::to_wstring(GetCurrentProcessId()));
}

/// Unpacks the package and installs from it. Everything that takes time; runs beside the window.
int unpackAndInstall(const std::vector<std::uint8_t>& package, bool onlyCheck) {
    const fs::path folder = temporaryFolder();
    if (folder.empty()) return kNotUnpacked;
    removeFolder(folder);
    const auto unpacked = update::extractZip(package, folder);
    const fs::path program = folder / kProgram;
    std::error_code ec;
    if (!unpacked || !fs::exists(program, ec)) {
        removeFolder(folder);
        return kNotUnpacked;
    }
    int result = kDone;
    if (!onlyCheck) {
        if (installedProgramAfter(program).empty()) result = kNotInstalled;
    }
    removeFolder(folder);
    return result;
}

/// A small note in the middle of the screen while the work runs, so a double
/// click is answered at once and nobody clicks a second time.
HWND showNote(HINSTANCE instance, const wchar_t* text, HFONT& font) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        font = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    const int width = 360;
    const int height = 96;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    const HWND note = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", text,
                                      WS_POPUP | WS_BORDER | SS_CENTER | SS_CENTERIMAGE, x, y, width, height, nullptr,
                                      nullptr, instance, nullptr);
    if (!note) return nullptr;
    if (font) SendMessageW(note, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ShowWindow(note, SW_SHOWNOACTIVATE);
    UpdateWindow(note);
    return note;
}

int makeSetup(const fs::path& own, const fs::path& zip, const fs::path& setupFile) {
    std::ifstream in(zip, std::ios::binary);
    if (!in) return kNoPackage;
    const std::vector<std::uint8_t> package((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return setup::writeSetup(own, package, setupFile) ? kDone : kNotWritten;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool quiet = false;
    bool check = false;
    std::vector<std::wstring> make;
    for (int i = 1; arguments && i < count; ++i) {
        const std::wstring_view argument(arguments[i]);
        if (argument == L"--quiet") quiet = true;
        else if (argument == L"--check") check = true;
        else if (argument == L"--make") {
            for (int k = i + 1; k < count; ++k) make.emplace_back(arguments[k]);
            make.insert(make.begin(), L"--make");
            break;
        }
    }
    if (arguments) LocalFree(arguments);

    const fs::path own = ownFile();
    if (!make.empty()) return make.size() == 3 ? makeSetup(own, make[1], make[2]) : kBadArguments;

    // A second double click while the first still works does nothing.
    const HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\VeloPlaySetup");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return kDone;

    const Texts& t = texts();
    const auto package = setup::readPayload(own);
    if (!package) {
        if (!quiet && !check) MessageBoxW(nullptr, t.incomplete, L"VeloPlay", MB_OK | MB_ICONWARNING);
        return kNoPackage;
    }

    // The files of a running VeloPlay cannot be replaced.
    while (!check && FindWindowW(kWindowClass, nullptr)) {
        if (quiet) return kStillRunning;
        if (MessageBoxW(nullptr, t.running, L"VeloPlay", MB_RETRYCANCEL | MB_ICONINFORMATION) != IDRETRY) return kStillRunning;
    }

    HFONT font = nullptr;
    const HWND note = quiet || check ? nullptr : showNote(instance, t.installing, font);

    std::atomic<int> result{kNotInstalled};
    std::thread work([&] { result = unpackAndInstall(*package, check); });
    const HANDLE worker = static_cast<HANDLE>(work.native_handle());
    for (;;) {
        // Keep the note alive (Windows greys out a window that answers nothing) until the work is done.
        if (MsgWaitForMultipleObjects(1, &worker, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0) break;
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    work.join();
    if (note) DestroyWindow(note);
    if (font) DeleteObject(font);

    if (result != kDone) {
        if (!quiet && !check) MessageBoxW(nullptr, t.failed, L"VeloPlay", MB_OK | MB_ICONWARNING);
        return result;
    }
    if (!quiet && !check) {
        const fs::path installed = installedProgram();
        const fs::path folder = installed.parent_path();
        ShellExecuteW(nullptr, L"open", installed.c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL);
    }
    return kDone;
}
