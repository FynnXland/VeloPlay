#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace veloplay::linuxos {

enum class UserDirectory { Pictures, Videos };

/// The user's folder for pictures or videos as the desktop names it
/// ($XDG_CONFIG_HOME/user-dirs.dirs, e.g. XDG_PICTURES_DIR="$HOME/Bilder"),
/// otherwise ~/Pictures or ~/Videos. Not created here.
[[nodiscard]] std::filesystem::path userDirectory(UserDirectory kind);

/// The same with the home and configuration folders given, for tests.
[[nodiscard]] std::filesystem::path userDirectory(UserDirectory kind, const std::filesystem::path& home,
                                                  const std::filesystem::path& configHome);

/// The value of `key` (XDG_PICTURES_DIR) in the text of a user-dirs.dirs file,
/// with $HOME expanded. Nullopt when the key is missing, its value is neither
/// "$HOME/..." nor absolute, or it is the home folder itself, which is how a
/// folder is switched off there.
[[nodiscard]] std::optional<std::filesystem::path> parseUserDirs(std::string_view text, std::string_view key,
                                                                 const std::filesystem::path& home);

/// The user's home folder: $HOME, else the password database.
[[nodiscard]] std::filesystem::path homeDirectory();

/// Opens an address in the browser or a folder in the file manager through
/// xdg-open. Runs it directly (no shell, so nothing in the text is
/// interpreted), in a session of its own, and does not wait; a thread of its
/// own collects it when it ends. False if it could not be started, or if the
/// text is empty or starts with "-" (it would be read as an option).
bool openWithDesktop(const std::string& urlOrPath);

} // namespace veloplay::linuxos
