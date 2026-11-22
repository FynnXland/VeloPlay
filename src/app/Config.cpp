#include "app/Config.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "net/Socket.hpp"

namespace veloplay {

namespace {

using json = nlohmann::json;

constexpr int kConfigVersion = 1;

std::string_view codecName(VideoCodec codec) { return codec == VideoCodec::H264 ? "h264" : "hevc"; }
std::string_view jitterName(JitterMode mode) { return mode == JitterMode::Stable ? "stable" : "low_latency"; }

/// Reads `key` into `target` if it exists and has a compatible type.
template <class T>
void read(const json& object, const char* key, T& target) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return;
    try {
        target = it->get<T>();
    } catch (const json::exception&) {
        // Wrong type: keep the default.
    }
}

const json& section(const json& root, const char* key) {
    static const json kEmpty = json::object();
    const auto it = root.find(key);
    return (it != root.end() && it->is_object()) ? *it : kEmpty;
}

Result<std::string> protectCredentials(const RegisteredConsole& console, ISecretProtector& protector) {
    const json secret{{"regist_key", console.registKey}, {"rp_key", toHex(console.rpKey)}};
    const auto blob = protector.protect(asBytes(secret.dump()));
    if (!blob) return blob.error();
    return base64Encode(*blob);
}

bool unprotectCredentials(std::string_view encoded, ISecretProtector& protector, RegisteredConsole& console) {
    const auto blob = base64Decode(encoded);
    if (!blob) return false;
    const auto plain = protector.unprotect(*blob);
    if (!plain) return false;

    const json secret = json::parse(asText(*plain), nullptr, false);
    if (!secret.is_object()) return false;

    std::string registKey, rpKeyHex;
    read(secret, "regist_key", registKey);
    read(secret, "rp_key", rpKeyHex);
    const auto rpKey = fromHex(rpKeyHex);
    if (registKey.empty() || !rpKey || rpKey->size() != console.rpKey.size()) return false;

    console.registKey = std::move(registKey);
    std::copy(rpKey->begin(), rpKey->end(), console.rpKey.begin());
    return true;
}

} // namespace

RegisteredConsole RegisteredConsole::from(const protocol::RegisteredHost& host, std::string_view address) {
    RegisteredConsole console;
    console.name = host.nickname.empty() ? "PS5" : host.nickname;
    console.hostId = host.hostId();
    console.lastAddress = std::string(address);
    console.registKey = host.registKey;
    console.rpKey = host.rpKey;
    console.rpKeyType = host.rpKeyType;
    return console;
}

void sanitize(AppConfig& config) {
    if (config.language != "de" && config.language != "en") config.language = "auto";

    sanitize(config.stream);
    config.graphics.frameGenerationFactor = std::clamp(config.graphics.frameGenerationFactor, 0, 4);
    config.graphics.superResolutionLevel = std::clamp(config.graphics.superResolutionLevel, 0, 2);
    config.graphics.sharpness = std::clamp(config.graphics.sharpness, 0.0f, 1.0f);
    config.graphics.look = std::clamp(config.graphics.look, 0, 2);
    config.graphics.lookStrength = std::clamp(config.graphics.lookStrength, 0.0f, 1.0f);
    config.network.statsCorner = std::clamp(config.network.statsCorner, 0, 3);
    config.controller.deadzone = std::clamp(config.controller.deadzone, 0.0f, 0.5f);
    input::sanitize(config.controller.keyboardMouse);
    {
        input::WheelSettings& wheel = config.controller.wheel;
        wheel.steering = std::clamp(wheel.steering, 0.5f, 3.0f);
        wheel.deadzone = std::clamp(wheel.deadzone, 0.0f, 0.2f);
        wheel.centering = std::clamp(wheel.centering, 0.0f, 1.0f);
        wheel.steerAxis = std::clamp(wheel.steerAxis, -1, 15);
        wheel.gas.axis = std::clamp(wheel.gas.axis, -1, 15);
        wheel.brake.axis = std::clamp(wheel.brake.axis, -1, 15);
        for (int& button : wheel.buttons) button = std::clamp(button, -1, 63);
    }
    if (config.remoteApi.port < 1024) config.remoteApi.port = 9876;

    std::erase_if(config.manualHosts, [](const std::string& host) { return !net::parseIpv4(host).has_value(); });
    std::sort(config.manualHosts.begin(), config.manualHosts.end());
    config.manualHosts.erase(std::unique(config.manualHosts.begin(), config.manualHosts.end()), config.manualHosts.end());

    std::erase_if(config.consoles, [](const RegisteredConsole& console) { return console.hostId.empty(); });
    for (RegisteredConsole& console : config.consoles) {
        if (console.link.bitrateKbps != 0) {
            console.link.bitrateKbps = std::clamp(console.link.bitrateKbps, kMinBitrateKbps, kAutoMaxBitrateKbps);
        }
        console.link.ceilingKbps = std::clamp(console.link.ceilingKbps, 0, kMaxBitrateKbps);
    }
    detectProfile(config);
}

// Raised when defaults change in a way that old files have to follow.
constexpr int kSettingsVersion = 5;

bool applyProfile(AppConfig& config, std::string_view profileId) {
    if (profileId == kProfileAuto) {
        // The stored stream settings stay as they are, for switching back.
        config.graphics.frameGenerationFactor = 0;
        config.activeProfile = std::string(kProfileAuto);
        return true;
    }
    const StreamProfile* profile = findProfile(profileId);
    if (!profile) return false;
    config.stream = profile->stream;
    config.graphics.frameGenerationFactor = profile->frameGenerationFactor;
    config.activeProfile = std::string(profile->id);
    return true;
}

StreamSettings effectiveStream(const AppConfig& config, std::string_view hostId) {
    if (!isAutomatic(config)) return config.stream;
    for (const RegisteredConsole& console : config.consoles) {
        if (console.hostId == hostId) return automaticStream(console.link);
    }
    return automaticStream(LinkMemory{});
}

void detectProfile(AppConfig& config) {
    if (isAutomatic(config)) return;
    for (const StreamProfile& profile : builtInProfiles()) {
        if (config.stream == profile.stream && config.graphics.frameGenerationFactor == profile.frameGenerationFactor) {
            config.activeProfile = std::string(profile.id);
            return;
        }
    }
    config.activeProfile = std::string(kProfileCustom);
}

Result<std::string> serializeConfig(const AppConfig& config, ISecretProtector& protector) {
    json root;
    root["version"] = kConfigVersion;
    root["settings_version"] = kSettingsVersion;
    root["language"] = config.language;
    root["active_profile"] = config.activeProfile;
    root["account_id"] = config.accountId;
    root["manual_hosts"] = config.manualHosts;

    root["stream"] = {
        {"width", config.stream.width},   {"height", config.stream.height},
        {"fps", config.stream.fps},       {"codec", codecName(config.stream.codec)},
        {"hdr", config.stream.hdr},       {"bitrate_kbps", config.stream.bitrateKbps},
    };
    root["graphics"] = {
        {"frame_generation_factor", config.graphics.frameGenerationFactor},
        {"super_resolution_level", config.graphics.superResolutionLevel},
        {"sharpness", config.graphics.sharpness},
        {"deband", config.graphics.deband},
        {"look", config.graphics.look},
        {"look_strength", config.graphics.lookStrength},
        {"amd_frc", config.graphics.amdFrc},
        {"vsync", config.graphics.vsync},
    };
    root["controller"] = {
        {"deadzone", config.controller.deadzone},
        {"rumble", config.controller.rumble},
        {"adaptive_triggers", config.controller.adaptiveTriggers},
        {"preferred_name", config.controller.preferredName},
    };
    {
        json keys = json::object();
        const input::KeyboardMouseSettings& kbm = config.controller.keyboardMouse;
        for (std::size_t i = 0; i < kbm.keys.size(); ++i) {
            keys[std::string(input::padControlName(static_cast<input::PadControl>(i)))] = kbm.keys[i];
        }
        json games = json::array();
        for (const input::GameKeyLayout& game : kbm.games) {
            json gameKeys = json::object();
            for (std::size_t i = 0; i < game.keys.size(); ++i) {
                gameKeys[std::string(input::padControlName(static_cast<input::PadControl>(i)))] = game.keys[i];
            }
            games.push_back({
                {"title_id", game.titleId},
                {"name", game.name},
                {"mouse_sensitivity", game.mouseSensitivity},
                {"invert_y", game.invertY},
                {"keys", std::move(gameKeys)},
            });
        }
        root["keyboard_mouse"] = {
            {"enabled", kbm.enabled},
            {"mouse_sensitivity", kbm.mouseSensitivity},
            {"invert_y", kbm.invertY},
            {"keys", std::move(keys)},
            {"games", std::move(games)},
        };
    }
    {
        const input::WheelSettings& wheel = config.controller.wheel;
        const auto pedal = [](const input::PedalAxis& axis) {
            return json{{"axis", axis.axis}, {"released", axis.released}, {"pressed", axis.pressed}};
        };
        root["wheel"] = {
            {"enabled", wheel.enabled},       {"device", wheel.device},       {"steer_axis", wheel.steerAxis},
            {"steer_center", wheel.steerCenter}, {"steering", wheel.steering}, {"deadzone", wheel.deadzone},
            {"gas", pedal(wheel.gas)},         {"brake", pedal(wheel.brake)},  {"buttons", wheel.buttons},
            {"centering", wheel.centering},
        };
    }
    root["remote_api"] = {
        {"enabled", config.remoteApi.enabled},
        {"port", config.remoteApi.port},
        {"token", config.remoteApi.token},
    };
    root["network"] = {
        {"jitter", jitterName(config.network.jitter)},
        {"stats_overlay", config.network.statsOverlay},
        {"stats_compact", config.network.statsCompact},
        {"stats_corner", config.network.statsCorner},
    };
    root["audio"] = {
        {"microphone", config.audio.microphone},
    };
    root["discord"] = {
        {"enabled", config.discord.enabled},
        {"show_game", config.discord.showGame},
    };
    root["window"] = {
        {"fullscreen", config.window.fullscreen},
    };

    root["updates"] = {
        {"check_at_startup", config.updates.checkAtStartup},
    };

    json consoles = json::array();
    for (const RegisteredConsole& console : config.consoles) {
        json entry{
            {"name", console.name},
            {"host_id", console.hostId},
            {"last_address", console.lastAddress},
            {"rp_key_type", console.rpKeyType},
            {"auto_bitrate_kbps", console.link.bitrateKbps},
            {"auto_ceiling_kbps", console.link.ceilingKbps},
        };
        // A console whose keys could not be read keeps its place in the list
        // (the user sees "pair again"), but there is nothing to encrypt.
        if (console.credentialsValid) {
            const auto credentials = protectCredentials(console, protector);
            if (!credentials) return credentials.error();
            entry["credentials"] = *credentials;
        } else if (!console.sealedCredentials.empty()) {
            entry["credentials"] = console.sealedCredentials;
        }
        consoles.push_back(std::move(entry));
    }
    root["consoles"] = std::move(consoles);
    // Names come from consoles and games as they sent them; broken UTF-8 there must not stop the
    // settings from being saved (dump() would throw).
    return root.dump(2, ' ', false, json::error_handler_t::replace);
}

Result<AppConfig> parseConfig(std::string_view text, ISecretProtector& protector) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object()) return Error{ErrorCode::InvalidData, "the configuration file is not valid JSON"};

    AppConfig config;
    read(root, "language", config.language);
    read(root, "active_profile", config.activeProfile);
    read(root, "account_id", config.accountId);
    read(root, "manual_hosts", config.manualHosts);

    const json& stream = section(root, "stream");
    read(stream, "width", config.stream.width);
    read(stream, "height", config.stream.height);
    read(stream, "fps", config.stream.fps);
    read(stream, "hdr", config.stream.hdr);
    read(stream, "bitrate_kbps", config.stream.bitrateKbps);
    std::string codec;
    read(stream, "codec", codec);
    config.stream.codec = codec == "h264" ? VideoCodec::H264 : VideoCodec::Hevc;

    const json& graphics = section(root, "graphics");
    read(graphics, "frame_generation_factor", config.graphics.frameGenerationFactor);
    read(graphics, "super_resolution_level", config.graphics.superResolutionLevel);
    read(graphics, "sharpness", config.graphics.sharpness);
    read(graphics, "deband", config.graphics.deband);
    read(graphics, "look", config.graphics.look);
    read(graphics, "look_strength", config.graphics.lookStrength);
    read(graphics, "amd_frc", config.graphics.amdFrc);
    read(graphics, "vsync", config.graphics.vsync);

    const json& controller = section(root, "controller");
    read(controller, "deadzone", config.controller.deadzone);
    read(controller, "rumble", config.controller.rumble);
    read(controller, "adaptive_triggers", config.controller.adaptiveTriggers);
    read(controller, "preferred_name", config.controller.preferredName);

    {
        const json& wheelJson = section(root, "wheel");
        input::WheelSettings& wheel = config.controller.wheel;
        read(wheelJson, "enabled", wheel.enabled);
        read(wheelJson, "device", wheel.device);
        read(wheelJson, "steer_axis", wheel.steerAxis);
        read(wheelJson, "steer_center", wheel.steerCenter);
        read(wheelJson, "steering", wheel.steering);
        read(wheelJson, "deadzone", wheel.deadzone);
        read(wheelJson, "centering", wheel.centering);
        const auto readPedal = [&](const char* key, input::PedalAxis& into) {
            const json& pedal = section(wheelJson, key);
            read(pedal, "axis", into.axis);
            read(pedal, "released", into.released);
            read(pedal, "pressed", into.pressed);
        };
        readPedal("gas", wheel.gas);
        readPedal("brake", wheel.brake);
        const auto buttons = wheelJson.find("buttons");
        if (buttons != wheelJson.end() && buttons->is_array()) {
            for (std::size_t i = 0; i < wheel.buttons.size() && i < buttons->size(); ++i) {
                if ((*buttons)[i].is_number_integer()) wheel.buttons[i] = (*buttons)[i].get<int>();
            }
        }
    }

    // Controls a file does not name keep their default key, so a newer version can add controls.
    const json& keyboardMouse = section(root, "keyboard_mouse");
    input::KeyboardMouseSettings& kbm = config.controller.keyboardMouse;
    read(keyboardMouse, "enabled", kbm.enabled);
    read(keyboardMouse, "mouse_sensitivity", kbm.mouseSensitivity);
    read(keyboardMouse, "invert_y", kbm.invertY);
    const auto readKeys = [](const json& from, std::array<int, input::kPadControlCount>& into) {
        const auto keys = from.find("keys");
        if (keys == from.end() || !keys->is_object()) return;
        for (const auto& [name, code] : keys->items()) {
            const auto control = input::parsePadControl(name);
            if (control && code.is_number_integer()) into[static_cast<std::size_t>(*control)] = code.get<int>();
        }
    };
    readKeys(keyboardMouse, kbm.keys);
    kbm.games.clear();
    const auto games = keyboardMouse.find("games");
    if (games != keyboardMouse.end() && games->is_array()) {
        for (const json& entry : *games) {
            if (!entry.is_object()) continue;
            input::GameKeyLayout game;
            read(entry, "title_id", game.titleId);
            read(entry, "name", game.name);
            read(entry, "mouse_sensitivity", game.mouseSensitivity);
            read(entry, "invert_y", game.invertY);
            readKeys(entry, game.keys);
            kbm.games.push_back(std::move(game));
        }
    }

    const json& remoteApi = section(root, "remote_api");
    read(remoteApi, "enabled", config.remoteApi.enabled);
    read(remoteApi, "port", config.remoteApi.port);
    read(remoteApi, "token", config.remoteApi.token);

    const json& network = section(root, "network");
    std::string jitter;
    read(network, "jitter", jitter);
    config.network.jitter = jitter == "stable" ? JitterMode::Stable : JitterMode::LowLatency;
    read(network, "stats_overlay", config.network.statsOverlay);
    read(network, "stats_compact", config.network.statsCompact);
    read(network, "stats_corner", config.network.statsCorner);
    read(section(root, "audio"), "microphone", config.audio.microphone);
    read(section(root, "discord"), "enabled", config.discord.enabled);
    read(section(root, "discord"), "show_game", config.discord.showGame);
    read(section(root, "window"), "fullscreen", config.window.fullscreen);

    read(section(root, "updates"), "check_at_startup", config.updates.checkAtStartup);

    const auto consoles = root.find("consoles");
    if (consoles != root.end() && consoles->is_array()) {
        for (const json& entry : *consoles) {
            if (!entry.is_object()) continue;
            RegisteredConsole console;
            read(entry, "name", console.name);
            read(entry, "host_id", console.hostId);
            read(entry, "last_address", console.lastAddress);
            read(entry, "rp_key_type", console.rpKeyType);
            read(entry, "auto_bitrate_kbps", console.link.bitrateKbps);
            read(entry, "auto_ceiling_kbps", console.link.ceilingKbps);
            std::string credentials;
            read(entry, "credentials", credentials);
            console.credentialsValid = unprotectCredentials(credentials, protector, console);
            if (!console.credentialsValid) console.sealedCredentials = credentials;
            config.consoles.push_back(std::move(console));
        }
    }

    // Versions up to 0.4 started at 40 and 50 Mbit/s, more than a console on
    // Wi-Fi can send. Files from then that still hold those values get the
    // current defaults; a rate the user chose stays.
    int version = 1;
    read(root, "settings_version", version);
    if (version < 2 && (config.stream.bitrateKbps == 40'000 || config.stream.bitrateKbps == 50'000)) {
        const StreamProfile* profile = findProfile(config.activeProfile);
        config.stream.bitrateKbps = profile ? profile->stream.bitrateKbps : StreamSettings{}.bitrateKbps;
    }

    // Up to 0.5 frame generation and upscaling were stored but did nothing,
    // so what a file from then holds was never a choice. The pairs the
    // profiles used become "automatic", which does the same, and upscaling
    // starts switched on.
    if (version < 3) {
        const int factor = config.graphics.frameGenerationFactor;
        if ((config.stream.fps <= 30 && factor == 2) || (config.stream.fps > 30 && factor == 1)) {
            config.graphics.frameGenerationFactor = 0;
        }
        config.graphics.superResolutionLevel = 1;
    }

    // Since 0.9 the program chooses what to ask the console for. Files from
    // before start with that once; the values they hold stay stored and come
    // back when "automatic" is switched off.
    if (version < 4) config.activeProfile = std::string(kProfileAuto);

    // Up to 0.17 the keys started out with run on Cross and jump on Square.
    // Games of today have jump on Cross and run on the left stick, and the
    // layout to start with follows them since 0.18. A file that still holds
    // the old start exactly was never changed and moves along; a layout
    // somebody made stays.
    if (version < 5) {
        input::KeyboardMouseSettings& keyboard = config.controller.keyboardMouse;
        if (keyboard.keys == input::firstKeyBindings()) keyboard.keys = input::defaultKeyBindings();
        for (input::GameKeyLayout& game : keyboard.games) {
            if (game.keys == input::firstKeyBindings()) game.keys = input::defaultKeyBindings();
        }
    }

    sanitize(config);
    return config;
}

ConfigStore::ConfigStore(std::filesystem::path path, ISecretProtector& protector)
    : path_(std::move(path)), protector_(protector) {}

Result<AppConfig> ConfigStore::load() {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        AppConfig config;
        sanitize(config);
        return config;
    }

    const auto readFile = [&](const std::filesystem::path& path) -> std::optional<std::string> {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        std::ostringstream content;
        content << file.rdbuf();
        return content.str();
    };
    const std::optional<std::string> text = readFile(path_);
    if (!text) return Error{ErrorCode::Io, "cannot open " + path_.string()};
    Result<AppConfig> parsed = parseConfig(*text, protector_);
    if (parsed) return parsed;

    // Unreadable (a hand edit gone wrong, an empty file after a power cut). Keep it where nothing
    // overwrites it; whatever is saved next would otherwise replace it with the defaults.
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::path aside = path_;
    aside.replace_filename(path_.stem().string() + ".broken-" + std::to_string(stamp) + path_.extension().string());
    std::filesystem::rename(path_, aside, ec);
    const std::string kept = ec ? std::string() : "; kept as " + aside.filename().string();
    std::filesystem::path backup = path_;
    backup += ".bak";
    if (const std::optional<std::string> earlier = readFile(backup)) {
        Result<AppConfig> restored = parseConfig(*earlier, protector_);
        if (restored) {
            spdlog::warn("Settings: {}{}; the copy from before the last save is used", parsed.error().message, kept);
            return restored;
        }
    }
    return Error{parsed.error().code, parsed.error().message + kept};
}

Result<void> ConfigStore::save(const AppConfig& config) {
    AppConfig copy = config;
    sanitize(copy);
    const auto text = serializeConfig(copy, protector_);
    if (!text) return text.error();

    std::error_code ec;
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);

    std::filesystem::path temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return Error{ErrorCode::Io, "cannot write " + temporary.string()};
        file << *text;
        file.flush();
        if (!file) return Error{ErrorCode::Io, "cannot write " + temporary.string()};
    }
    // The file before this one stays as a copy, for load() to fall back on.
    if (std::filesystem::exists(path_, ec) && std::filesystem::file_size(path_, ec) > 0 && !ec) {
        std::filesystem::path backup = path_;
        backup += ".bak";
        std::filesystem::copy_file(path_, backup, std::filesystem::copy_options::overwrite_existing, ec);
    }
    ec.clear();
    std::filesystem::rename(temporary, path_, ec);
    if (ec) return Error{ErrorCode::Io, "cannot replace " + path_.string() + ": " + ec.message()};
    return {};
}

std::filesystem::path defaultConfigDirectory() {
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "VeloPlay";
    }
    return std::filesystem::current_path();
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "veloplay";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "veloplay";
    }
    return std::filesystem::current_path();
#endif
}

} // namespace veloplay
