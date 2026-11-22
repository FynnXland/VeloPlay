// The local connection to the Discord app: a named pipe Discord's desktop
// client opens for applications on the same PC (Discord's RPC over IPC).
#include "app/DiscordPresence.hpp"

#include <windows.h>

#include <string>

namespace veloplay::discord {

namespace {

class WindowsPipe final : public IPipe {
public:
    ~WindowsPipe() override { close(); }

    bool open() override {
        close();
        // Discord takes the first free of ten names; a second Discord (Canary, PTB) the next.
        for (int i = 0; i < 10; ++i) {
            const std::wstring name = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(i);
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }

    void close() override {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }

    bool write(std::span<const std::uint8_t> bytes) override {
        if (pipe_ == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        return WriteFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
               written == bytes.size();
    }

    std::optional<std::vector<std::uint8_t>> read() override {
        if (pipe_ == INVALID_HANDLE_VALUE) return std::nullopt;
        // Only what is there already: the window must never wait for Discord.
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return std::nullopt;
        std::vector<std::uint8_t> out(available);
        if (available == 0) return out;
        DWORD got = 0;
        if (!ReadFile(pipe_, out.data(), available, &got, nullptr)) return std::nullopt;
        out.resize(got);
        return out;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

} // namespace

std::unique_ptr<IPipe> createPipe() { return std::make_unique<WindowsPipe>(); }

} // namespace veloplay::discord
