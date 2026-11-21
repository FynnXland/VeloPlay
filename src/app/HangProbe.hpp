#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

// Windows only. Asks Windows whether it considers the window hung, and
// collects what explains it when it does.
//
// The stall watch (util/StallWatch) goes by whether the window's loop comes
// around. That is not the whole picture: with 0.5.2 a window was "not
// responding" for Windows while its loop ran and drew thirty pictures a
// second (issue 3). So this asks the question the way Windows answers it.

namespace veloplay::diag {

/// What the window's loop last did with a message. Written by the window's
/// thread, read by the probe.
struct MessageTrace {
    std::atomic<unsigned> lastMessage{0};        // id of the message handled last
    std::atomic<unsigned long long> lastTick{0}; // GetTickCount64() when it was taken from the queue
    std::atomic<unsigned long long> count{0};    // messages handled since the start

    void note(unsigned message) noexcept {
        lastMessage.store(message, std::memory_order_relaxed);
        lastTick.store(GetTickCount64(), std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
    }
};

/// True if Windows treats the window as not responding, or if the window
/// does not answer an empty message within `timeoutMs`. Call from a thread
/// other than the window's.
[[nodiscard]] bool windowLooksHung(HWND window, unsigned timeoutMs);

/// Lines for the log: what the window's thread holds (focus, capture, menu
/// or move loops), every window of this process with its thread and owner,
/// and windows of other programs that hang off ours. A window on another
/// thread or in another program that is tied to ours shares its input queue
/// with us; if that one stops reading input, ours stops getting any.
[[nodiscard]] std::vector<std::string> describeWindows(HWND window);

} // namespace veloplay::diag
