#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/AutoStream.hpp"
#include "app/StreamProfile.hpp"
#include "input/KeyboardMouse.hpp"
#include "protocol/Crypto.hpp"
#include "protocol/Registration.hpp"
#include "util/Bytes.hpp"
#include "util/Result.hpp"

// User settings and paired consoles, stored as JSON. The keys a console hands
// out during pairing are encrypted before they reach the file.

namespace veloplay {

/// Encrypts small secrets for storage on this machine. Windows builds use
/// DPAPI (see DpapiProtector), which ties the data to the Windows user account.
class ISecretProtector {
public:
    virtual ~ISecretProtector() = default;
    virtual Result<Bytes> protect(std::span<const std::uint8_t> plaintext) = 0;
    virtual Result<Bytes> unprotect(std::span<const std::uint8_t> ciphertext) = 0;
};

struct GraphicsSettings {
    // 0 = automatic (a 30 fps stream is doubled, a 60 fps stream shown as it
    // is), 1 = off, 2..4 = that many pictures per picture from the console.
    int frameGenerationFactor = 0;
    // 0 = off (soft enlargement), 1 = Lanczos, 2 = RTX Video Super Resolution
    // (NVIDIA only; Lanczos where the card does not offer it)
    int superResolutionLevel = 1;
    float sharpness = 0.2f;        // 0.0 .. 1.0
    bool vsync = true;

    friend bool operator==(const GraphicsSettings&, const GraphicsSettings&) = default;
};

struct ControllerSettings {
    float deadzone = 0.08f; // fraction of full stick travel
    bool rumble = true;
    bool adaptiveTriggers = true;
    std::string preferredName; // empty = pick automatically
    input::KeyboardMouseSettings keyboardMouse; // playing with keys and mouse instead

    friend bool operator==(const ControllerSettings&, const ControllerSettings&) = default;
};

struct RemoteApiSettings {
    bool enabled = false;
    std::uint16_t port = 9876;
    std::string token; // optional shared secret

    friend bool operator==(const RemoteApiSettings&, const RemoteApiSettings&) = default;
};

enum class JitterMode { LowLatency, Stable };

struct NetworkSettings {
    JitterMode jitter = JitterMode::LowLatency;
    bool statsOverlay = true;
    /// With the display on: only the few figures everybody wants (pictures per second, link), not all of them.
    bool statsCompact = false;

    friend bool operator==(const NetworkSettings&, const NetworkSettings&) = default;
};

struct AudioSettings {
    /// Sends the default microphone's sound to the console, for voice chat. Off unless the user asks for it.
    bool microphone = false;

    friend bool operator==(const AudioSettings&, const AudioSettings&) = default;
};

struct WindowSettings {
    /// The window fills the screen, without a frame. F11 and the button in the bar switch it.
    bool fullscreen = true;

    friend bool operator==(const WindowSettings&, const WindowSettings&) = default;
};

struct UpdateSettings {
    bool checkAtStartup = true;

    friend bool operator==(const UpdateSettings&, const UpdateSettings&) = default;
};

/// A console this PC is paired with.
struct RegisteredConsole {
    std::string name;
    std::string hostId;      // MAC, 12 uppercase hex digits
    std::string lastAddress; // last known IPv4, used when discovery finds nothing
    std::string registKey;   // secret
    crypto::Key128 rpKey{};  // secret
    std::uint32_t rpKeyType = 0;
    LinkMemory link;         // what "automatic" learned about the way to this console
    /// False when the stored keys could not be decrypted, for example after the
    /// file was copied to another PC. The console then needs to be paired again.
    bool credentialsValid = true;

    static RegisteredConsole from(const protocol::RegisteredHost& host, std::string_view address);

    friend bool operator==(const RegisteredConsole&, const RegisteredConsole&) = default;
};

struct AppConfig {
    std::string language = "auto"; // "auto", "de", "en"
    std::string activeProfile{kProfileAuto};
    StreamSettings stream{};
    GraphicsSettings graphics{};
    ControllerSettings controller{};
    RemoteApiSettings remoteApi{};
    NetworkSettings network{};
    AudioSettings audio{};
    WindowSettings window{};
    UpdateSettings updates{};
    std::string accountId; // PSN account ID, base64 notation
    std::vector<std::string> manualHosts; // IPv4 addresses probed in addition to the broadcast
    std::vector<RegisteredConsole> consoles;

    friend bool operator==(const AppConfig&, const AppConfig&) = default;
};

/// Brings every value into its valid range. Called after loading and before saving.
void sanitize(AppConfig& config);

/// Applies a built-in profile. Returns false for unknown ids.
bool applyProfile(AppConfig& config, std::string_view profileId);

/// Sets activeProfile to the built-in profile matching the current values, or
/// to "custom" if none does. "Automatic" stays: it does not depend on the values.
void detectProfile(AppConfig& config);

[[nodiscard]] inline bool isAutomatic(const AppConfig& config) { return config.activeProfile == kProfileAuto; }

/// What a session with this console asks for: the program's choice with
/// "automatic", the stored settings otherwise. An unknown console gets the
/// choice for a link nothing is known about.
[[nodiscard]] StreamSettings effectiveStream(const AppConfig& config, std::string_view hostId);

Result<std::string> serializeConfig(const AppConfig& config, ISecretProtector& protector);

/// Unknown keys are ignored and missing ones keep their defaults, so files
/// written by other versions still load.
Result<AppConfig> parseConfig(std::string_view text, ISecretProtector& protector);

class ConfigStore {
public:
    ConfigStore(std::filesystem::path path, ISecretProtector& protector);

    /// A missing file yields the defaults.
    Result<AppConfig> load();

    /// Writes to a temporary file first, then renames it over the old one.
    Result<void> save(const AppConfig& config);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    ISecretProtector& protector_;
};

/// %APPDATA%\VeloPlay on Windows, $XDG_CONFIG_HOME/veloplay elsewhere.
std::filesystem::path defaultConfigDirectory();

#ifdef _WIN32
/// DPAPI-backed protector. The data can only be read by the same Windows user.
std::unique_ptr<ISecretProtector> createDpapiProtector();
#endif

} // namespace veloplay
