// The socket to Discord against a listening socket in a temporary folder, as
// Discord's client opens it.
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "linux/DiscordPipeLinux.hpp"

using namespace veloplay;
namespace fs = std::filesystem;

namespace {

fs::path freshSocketFolder(const char* name) {
    // Short: a Unix socket path has room for about 107 bytes.
    const fs::path folder = fs::path("/tmp") / ("vp-" + std::to_string(::getpid()) + "-" + name);
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

/// Discord's side: listens on `path` and accepts one connection.
struct FakeDiscord {
    int listener = -1;
    int client = -1;

    explicit FakeDiscord(const fs::path& path) {
        listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        EXPECT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(::listen(listener, 1), 0);
    }
    ~FakeDiscord() {
        if (client >= 0) ::close(client);
        ::close(listener);
    }
    void accept() { client = ::accept(listener, nullptr, nullptr); }
    std::string receive(std::size_t size) {
        std::string got(size, '\0');
        std::size_t have = 0;
        while (have < size) {
            const ssize_t n = ::recv(client, got.data() + have, size - have, 0);
            if (n <= 0) break;
            have += static_cast<std::size_t>(n);
        }
        got.resize(have);
        return got;
    }
};

std::vector<std::uint8_t> bytesOf(const std::string& text) {
    return {text.begin(), text.end()};
}

/// What read() delivers within a moment: it never waits, so ask a few times.
std::optional<std::vector<std::uint8_t>> readSoon(discord::IPipe& pipe, std::size_t want) {
    std::vector<std::uint8_t> all;
    for (int i = 0; i < 100; ++i) {
        auto got = pipe.read();
        if (!got) return std::nullopt;
        all.insert(all.end(), got->begin(), got->end());
        if (all.size() >= want) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return all;
}

} // namespace

TEST(LinuxDiscordPipe, TalksToDiscordAndNoticesWhenItGoes) {
    const fs::path empty = freshSocketFolder("none");
    const fs::path flatpak = freshSocketFolder("run");
    fs::create_directories(flatpak / "app" / "com.discordapp.Discord");
    // The first two names are taken by something that does not listen (a stale socket file of an old Discord).
    std::ofstream(flatpak / "app" / "com.discordapp.Discord" / "discord-ipc-0") << "";
    FakeDiscord discord(flatpak / "app" / "com.discordapp.Discord" / "discord-ipc-1");

    const auto pipe = linuxos::createDiscordPipe({empty, flatpak / "app" / "com.discordapp.Discord"});
    EXPECT_FALSE(pipe->write(bytesOf("x"))); // not open yet
    EXPECT_FALSE(pipe->read().has_value());
    ASSERT_TRUE(pipe->open());
    discord.accept();
    ASSERT_GE(discord.client, 0);

    const auto empty_ = pipe->read();
    ASSERT_TRUE(empty_.has_value()); // nothing yet, but connected
    EXPECT_TRUE(empty_->empty());

    ASSERT_TRUE(pipe->write(bytesOf("handshake")));
    EXPECT_EQ(discord.receive(9), "handshake");
    ASSERT_EQ(::send(discord.client, "READY", 5, 0), 5);
    const auto ready = readSoon(*pipe, 5);
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(std::string(ready->begin(), ready->end()), "READY");

    // Discord quits: reading says the connection broke, and writing fails without SIGPIPE.
    ::close(discord.client);
    discord.client = -1;
    EXPECT_FALSE(readSoon(*pipe, 1).has_value());
    pipe->write(bytesOf("after"));
    EXPECT_FALSE(pipe->write(bytesOf("after")));
    pipe->close();

    fs::remove_all(empty);
    fs::remove_all(flatpak);
}

TEST(LinuxDiscordPipe, FindsNothingWhereDiscordIsNot) {
    const fs::path folder = freshSocketFolder("absent");
    EXPECT_FALSE(linuxos::createDiscordPipe({folder})->open());
    EXPECT_FALSE(linuxos::createDiscordPipe({})->open());
    fs::remove_all(folder);
}

TEST(LinuxDiscordPipe, LooksWhereDiscordAndItsPackagesPutTheSocket) {
    const auto folders = linuxos::discordSocketFolders();
    ASSERT_FALSE(folders.empty());
    EXPECT_NE(std::find(folders.begin(), folders.end(), fs::path("/tmp")), folders.end());
    EXPECT_NE(std::find(folders.begin(), folders.end(), fs::path("/tmp/app/com.discordapp.Discord")), folders.end());
    EXPECT_NE(std::find(folders.begin(), folders.end(), fs::path("/tmp/snap.discord")), folders.end());

    const auto paths = linuxos::discordSocketPaths({"/run/user/1000", fs::path("/") / std::string(120, 'x')});
    ASSERT_EQ(paths.size(), 10u); // the folder too long for a socket is left out
    EXPECT_EQ(paths.front(), fs::path("/run/user/1000/discord-ipc-0"));
    EXPECT_EQ(paths.back(), fs::path("/run/user/1000/discord-ipc-9"));
}
