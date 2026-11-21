#include "util/ThreadStack.hpp"

#include <windows.h>

#include <dbghelp.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace veloplay::diag {

namespace {

constexpr std::size_t kMaxFrames = 40;

/// Copies the return addresses of a thread while it is stopped. Between
/// stopping and resuming nothing here may take a lock the stopped thread
/// could hold, so: no allocation, no logging, no symbol lookup.
std::size_t walk(HANDLE thread, std::array<DWORD64, kMaxFrames>& frames) {
#if defined(_M_X64) || defined(__x86_64__)
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return 0;
    std::size_t count = 0;
    CONTEXT context;
    std::memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(thread, &context)) {
        while (count < frames.size() && context.Rip != 0) {
            frames[count++] = context.Rip;
            DWORD64 imageBase = 0;
            const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function) {
                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                 &establisherFrame, nullptr);
                continue;
            }
            // No unwind data. For the innermost frame that means a leaf
            // function: it has not moved the stack pointer, so the return
            // address is on top. Anywhere else the trail ends here.
            if (count != 1) break;
            DWORD64 returnAddress = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp), &returnAddress,
                                   sizeof(returnAddress), nullptr)) {
                break;
            }
            context.Rip = returnAddress;
            context.Rsp += sizeof(DWORD64);
        }
    }
    ResumeThread(thread);
    return count;
#else
    (void)thread;
    (void)frames;
    return 0;
#endif
}

std::mutex gSymbolMutex; // the symbol functions of dbghelp are not thread-safe
bool gSymbolsReady = false;
bool gSymbolsTried = false;

std::string moduleOf(DWORD64 address, DWORD64& base) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        !module) {
        return {};
    }
    base = reinterpret_cast<DWORD64>(module);
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    // The file name only: the folder can contain the user's name.
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
    }
    char utf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return utf8;
}

std::string describe(DWORD64 address) {
    DWORD64 base = 0;
    const std::string module = moduleOf(address, base);
    char text[640] = {};
    int written = 0;
    if (module.empty()) {
        written = std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    } else {
        written = std::snprintf(text, sizeof(text), "%s+0x%llx", module.c_str(),
                                static_cast<unsigned long long>(address - base));
    }
    if (gSymbolsReady && written > 0 && static_cast<std::size_t>(written) < sizeof(text)) {
        // Without debug files this finds the nearest exported function, which
        // is enough to tell SDL_Delay from Present. Far from the address it
        // is a guess; the offset shows how far.
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
            std::snprintf(text + written, sizeof(text) - static_cast<std::size_t>(written), " (%s+0x%llx)", symbol->Name,
                          static_cast<unsigned long long>(displacement));
        }
    }
    return text;
}

} // namespace

ThreadStack::~ThreadStack() {
    if (thread_) CloseHandle(static_cast<HANDLE>(thread_));
}

void ThreadStack::watchCurrentThread() {
    if (thread_) CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                         GetCurrentThreadId());
}

std::vector<std::string> ThreadStack::capture() const {
    const HANDLE thread = static_cast<HANDLE>(thread_);
    // A thread that stops itself never wakes up again.
    if (!thread || GetThreadId(thread) == GetCurrentThreadId()) return {};

    std::array<DWORD64, kMaxFrames> frames{};
    const std::size_t count = walk(thread, frames);

    std::vector<std::string> lines;
    lines.reserve(count);
    std::lock_guard lock(gSymbolMutex);
    if (!gSymbolsTried) {
        gSymbolsTried = true;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        gSymbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    } else if (gSymbolsReady) {
        SymRefreshModuleList(GetCurrentProcess()); // libraries loaded since the last time
    }
    for (std::size_t i = 0; i < count; ++i) lines.push_back(describe(frames[i]));
    return lines;
}

} // namespace veloplay::diag
