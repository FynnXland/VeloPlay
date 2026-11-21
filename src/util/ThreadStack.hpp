#pragma once

#include <string>
#include <vector>

// Windows only. Reads where another thread of this process is right now, as
// a list of return addresses with the module and the nearest known function.
// Used when the window stops responding: the log then shows in which library
// the thread waits.

namespace veloplay::diag {

class ThreadStack {
public:
    ThreadStack() = default;
    ~ThreadStack();
    ThreadStack(const ThreadStack&) = delete;
    ThreadStack& operator=(const ThreadStack&) = delete;

    /// Remembers the calling thread as the one to look at.
    void watchCurrentThread();

    /// One line per frame, innermost first, for example
    /// "ntdll.dll+0x9d5f4 (NtDelayExecution+0x14)". Empty when the thread is
    /// not known, is the caller itself, or cannot be read. The thread is
    /// stopped only while its addresses are copied.
    [[nodiscard]] std::vector<std::string> capture() const;

private:
    void* thread_ = nullptr; // HANDLE
};

} // namespace veloplay::diag
