// The local connection to the Discord app on Linux: a Unix socket that
// Discord's desktop client opens for applications (Discord's RPC over IPC).
// The same frames as the named pipe on Windows (DiscordPipeWin.cpp).
#include "linux/DiscordPipeLinux.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace veloplay::linuxos {

namespace {

namespace fs = std::filesystem;

class UnixPipe final : public discord::IPipe {
public:
    explicit UnixPipe(std::vector<fs::path> folders) : folders_(std::move(folders)) {}
    ~UnixPipe() override { close(); }

    bool open() override {
        close();
        // Discord takes the first free of ten names; a second Discord (Canary, PTB) the next.
        for (const fs::path& path : discordSocketPaths(folders_)) {
            const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (socket < 0) return false;
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            const std::string text = path.string();
            std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
            // On a Unix socket this answers at once: connected, nobody listening, or no such file.
            if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                socket_ = socket;
                return true;
            }
            ::close(socket);
        }
        return false;
    }

    void close() override {
        if (socket_ >= 0) ::close(socket_);
        socket_ = -1;
    }

    bool write(std::span<const std::uint8_t> bytes) override {
        if (socket_ < 0) return false;
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t now = ::send(socket_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (now > 0) {
                sent += static_cast<std::size_t>(now);
                continue;
            }
            if (now < 0 && errno == EINTR) continue;
            if (now < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Discord reads slowly for a moment; the window must not wait long for it.
                pollfd wait{socket_, POLLOUT, 0};
                if (::poll(&wait, 1, 100) > 0) continue;
            }
            close();
            return false;
        }
        return true;
    }

    std::optional<std::vector<std::uint8_t>> read() override {
        if (socket_ < 0) return std::nullopt;
        // Only what is there already: the window must never wait for Discord.
        std::vector<std::uint8_t> out;
        std::uint8_t chunk[4096];
        while (true) {
            const ssize_t got = ::recv(socket_, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (got > 0) {
                out.insert(out.end(), chunk, chunk + got);
                continue;
            }
            if (got < 0 && errno == EINTR) continue;
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return out;
            // 0: Discord closed the connection. Anything else: it broke.
            return std::nullopt;
        }
    }

private:
    std::vector<fs::path> folders_;
    int socket_ = -1;
};

} // namespace

std::vector<fs::path> discordSocketFolders() {
    std::vector<fs::path> bases;
    for (const char* name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (const char* value = std::getenv(name); value && *value == '/') bases.emplace_back(value);
    }
    bases.emplace_back("/tmp");
    std::vector<fs::path> folders;
    const auto add = [&folders](const fs::path& folder) {
        for (const fs::path& known : folders) {
            if (known == folder) return;
        }
        folders.push_back(folder);
    };
    for (const fs::path& base : bases) {
        add(base);
        for (const char* package : {"app/com.discordapp.Discord", "app/com.discordapp.DiscordCanary", "snap.discord",
                                    "snap.discord-canary"}) {
            add(base / package);
        }
    }
    return folders;
}

std::vector<fs::path> discordSocketPaths(const std::vector<fs::path>& folders) {
    std::vector<fs::path> paths;
    for (const fs::path& folder : folders) {
        for (int i = 0; i < 10; ++i) {
            fs::path path = folder / ("discord-ipc-" + std::to_string(i));
            if (path.string().size() >= sizeof(sockaddr_un::sun_path)) continue;
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::unique_ptr<discord::IPipe> createDiscordPipe(std::vector<fs::path> folders) {
    return std::make_unique<UnixPipe>(std::move(folders));
}

} // namespace veloplay::linuxos

namespace veloplay::discord {
std::unique_ptr<IPipe> createPipe() {
    return linuxos::createDiscordPipe(linuxos::discordSocketFolders());
}
} // namespace veloplay::discord
