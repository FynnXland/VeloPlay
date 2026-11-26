#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "app/DiscordPresence.hpp"

namespace veloplay::linuxos {

/// The folders Discord's desktop client puts its socket into, in the order
/// they are tried: $XDG_RUNTIME_DIR, $TMPDIR, $TMP, $TEMP and /tmp, each also
/// with the subfolders of the Flatpak (app/com.discordapp.Discord, and Canary)
/// and Snap (snap.discord, snap.discord-canary) packages.
[[nodiscard]] std::vector<std::filesystem::path> discordSocketFolders();

/// discord-ipc-0 to -9 in each folder; paths too long for a Unix socket are left out.
[[nodiscard]] std::vector<std::filesystem::path> discordSocketPaths(const std::vector<std::filesystem::path>& folders);

/// The connection to Discord over its Unix socket, trying `folders` (for tests;
/// discord::createPipe() uses discordSocketFolders()). Reads never wait, writes
/// never raise SIGPIPE and give up after 100 ms on a full socket, closing it:
/// half a frame cannot be repaired.
std::unique_ptr<discord::IPipe> createDiscordPipe(std::vector<std::filesystem::path> folders);

} // namespace veloplay::linuxos
