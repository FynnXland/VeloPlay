#include "app/HangProbe.hpp"

#include <cstdio>

namespace veloplay::diag {

namespace {

std::string toUtf8(const wchar_t* text) {
    char buffer[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
    return buffer;
}

std::string classOf(HWND window) {
    if (!window) return "none";
    wchar_t name[128] = {};
    GetClassNameW(window, name, 128);
    return toUtf8(name);
}

/// File name of the program a process runs; the folder can hold the user's name.
std::string programOf(DWORD processId) {
    std::string result = "?";
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return result;
    wchar_t path[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        const wchar_t* name = path;
        for (DWORD i = 0; i < length; ++i) {
            if (path[i] == L'\\') name = path + i + 1;
        }
        result = toUtf8(name);
    }
    CloseHandle(process);
    return result;
}

std::string describe(HWND window, const char* role) {
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    const HWND owner = GetWindow(window, GW_OWNER);
    const HWND parent = GetAncestor(window, GA_PARENT);
    char line[512] = {};
    std::snprintf(line, sizeof(line), "%s %p class \"%s\" thread %lu%s%s, owner \"%s\", parent \"%s\"%s", role,
                  static_cast<void*>(window), classOf(window).c_str(), static_cast<unsigned long>(threadId),
                  IsWindowVisible(window) ? ", visible" : "", IsWindowEnabled(window) ? "" : ", disabled",
                  classOf(owner).c_str(), parent == GetDesktopWindow() ? "desktop" : classOf(parent).c_str(),
                  IsHungAppWindow(window) ? ", HUNG" : "");
    std::string text = line;
    if (processId != GetCurrentProcessId()) text += ", program " + programOf(processId);
    return text;
}

struct Collector {
    HWND main = nullptr;
    std::vector<std::string>* lines = nullptr;
};

BOOL CALLBACK onTopLevel(HWND window, LPARAM parameter) {
    auto* collector = reinterpret_cast<Collector*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == GetCurrentProcessId()) {
        collector->lines->push_back(describe(window, "own window"));
    } else if (GetWindow(window, GW_OWNER) == collector->main) {
        collector->lines->push_back(describe(window, "foreign window owned by ours"));
    }
    return collector->lines->size() < 80 ? TRUE : FALSE;
}

BOOL CALLBACK onChild(HWND window, LPARAM parameter) {
    auto* collector = reinterpret_cast<Collector*>(parameter);
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) {
        collector->lines->push_back(describe(window, "foreign child window"));
    } else if (threadId != GetWindowThreadProcessId(collector->main, nullptr)) {
        collector->lines->push_back(describe(window, "child window on another thread"));
    }
    return collector->lines->size() < 80 ? TRUE : FALSE;
}

} // namespace

bool windowLooksHung(HWND window, unsigned timeoutMs) {
    if (!window || !IsWindow(window)) return false;
    if (IsHungAppWindow(window)) return true;
    DWORD_PTR answer = 0;
    // An empty message. It is answered the next time the window's thread
    // asks for messages, which a healthy loop does many times a second.
    return SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, timeoutMs, &answer) == 0;
}

std::vector<std::string> describeWindows(HWND window) {
    std::vector<std::string> lines;
    const DWORD threadId = GetWindowThreadProcessId(window, nullptr);

    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(threadId, &info)) {
        char line[512] = {};
        std::snprintf(line, sizeof(line),
                      "window thread %lu: active \"%s\", focus \"%s\", capture \"%s\", menu owner \"%s\", move/size \"%s\", "
                      "flags 0x%lx%s%s%s",
                      static_cast<unsigned long>(threadId), classOf(info.hwndActive).c_str(),
                      classOf(info.hwndFocus).c_str(), classOf(info.hwndCapture).c_str(),
                      classOf(info.hwndMenuOwner).c_str(), classOf(info.hwndMoveSize).c_str(),
                      static_cast<unsigned long>(info.flags), (info.flags & GUI_INMENUMODE) ? " in-menu" : "",
                      (info.flags & GUI_INMOVESIZE) ? " in-move-size" : "",
                      (info.flags & GUI_SYSTEMMENUMODE) ? " system-menu" : "");
        lines.emplace_back(line);
    }
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    lines.push_back("foreground window: class \"" + classOf(foreground) + "\"" +
                    (foreground && foregroundProcess != GetCurrentProcessId()
                         ? ", program " + programOf(foregroundProcess)
                         : std::string(foreground ? ", ours" : "")));

    Collector collector{window, &lines};
    EnumWindows(&onTopLevel, reinterpret_cast<LPARAM>(&collector));
    EnumChildWindows(window, &onChild, reinterpret_cast<LPARAM>(&collector));
    return lines;
}

} // namespace veloplay::diag
