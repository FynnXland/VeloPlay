#include "linux/DesktopLinux.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <spdlog/spdlog.h>

extern char** environ;

namespace veloplay::linuxos {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

/// A shell-style value as xdg-user-dirs writes it: in double quotes, with backslash escapes.
std::string unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) ++i;
        out += value[i];
    }
    return out;
}

std::filesystem::path withoutTrailingSlash(const std::filesystem::path& path) {
    std::filesystem::path normal = path.lexically_normal();
    if (normal.has_relative_path() && normal.filename().empty()) normal = normal.parent_path();
    return normal;
}

std::filesystem::path fromEnvironment(const char* name) {
    const char* value = std::getenv(name);
    return value && *value == '/' ? std::filesystem::path(value) : std::filesystem::path{};
}

} // namespace

std::optional<std::filesystem::path> parseUserDirs(std::string_view text, std::string_view key,
                                                   const std::filesystem::path& home) {
    std::optional<std::filesystem::path> found;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        std::string_view entry = trim(line);
        if (entry.empty() || entry.front() == '#') continue;
        const std::size_t equals = entry.find('=');
        if (equals == std::string_view::npos || trim(entry.substr(0, equals)) != key) continue;
        const std::string value = unquote(trim(entry.substr(equals + 1)));
        std::filesystem::path path;
        for (const std::string_view prefix : {std::string_view("$HOME"), std::string_view("${HOME}")}) {
            if (value.starts_with(prefix) && (value.size() == prefix.size() || value[prefix.size()] == '/')) {
                const std::string rest = value.substr(prefix.size());
                path = rest.empty() ? home : home / std::filesystem::path(rest).relative_path();
                break;
            }
        }
        if (path.empty() && !value.empty() && value.front() == '/') path = value;
        if (path.empty()) {
            found.reset(); // a later line counts, as in the shell; a broken one leaves nothing
            continue;
        }
        // Set to the home folder itself means "switched off".
        if (withoutTrailingSlash(path) == withoutTrailingSlash(home)) {
            found.reset();
            continue;
        }
        found = withoutTrailingSlash(path);
    }
    return found;
}

std::filesystem::path homeDirectory() {
    if (std::filesystem::path home = fromEnvironment("HOME"); !home.empty()) return home;
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir) return user->pw_dir;
    return "/tmp";
}

std::filesystem::path userDirectory(UserDirectory kind, const std::filesystem::path& home,
                                    const std::filesystem::path& configHome) {
    const char* key = kind == UserDirectory::Pictures ? "XDG_PICTURES_DIR" : "XDG_VIDEOS_DIR";
    std::ifstream file(configHome / "user-dirs.dirs");
    if (file) {
        std::ostringstream text;
        text << file.rdbuf();
        if (auto chosen = parseUserDirs(text.str(), key, home)) return *chosen;
    }
    return home / (kind == UserDirectory::Pictures ? "Pictures" : "Videos");
}

std::filesystem::path userDirectory(UserDirectory kind) {
    const std::filesystem::path home = homeDirectory();
    std::filesystem::path config = fromEnvironment("XDG_CONFIG_HOME");
    if (config.empty()) config = home / ".config";
    return userDirectory(kind, home, config);
}

bool openWithDesktop(const std::string& urlOrPath) {
    if (urlOrPath.empty() || urlOrPath.front() == '-') return false;

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    // Nothing to read from us; and the browser it may start must not inherit our sockets and files.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
#endif
    // A session of its own: a browser started by it does not end with this program or get its signals.
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attributes, &none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE); // the program ignores it; xdg-open's shell should not
    posix_spawnattr_setsigdefault(&attributes, &defaults);

    std::string program = "xdg-open";
    std::string argument = urlOrPath;
    char* arguments[] = {program.data(), argument.data(), nullptr};
    pid_t child = 0;
    const int failed = posix_spawnp(&child, program.c_str(), &actions, &attributes, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (failed != 0) {
        spdlog::warn("Desktop: xdg-open could not be started ({})", failed);
        return false;
    }
    // Collected when it ends, so it does not linger as a zombie; nobody waits for the answer.
    std::thread([child] {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            spdlog::warn("Desktop: xdg-open ended with {}", WEXITSTATUS(status));
        }
    }).detach();
    return true;
}

} // namespace veloplay::linuxos
