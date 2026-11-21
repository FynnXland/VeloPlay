#include "input/RemoteControl.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace veloplay::input {

namespace {

using json = nlohmann::json;
using std::chrono::milliseconds;

constexpr int kDefaultStateTtlMs = 250;
constexpr int kMaxStateTtlMs = 5000;
constexpr int kDefaultPressMs = 100;
constexpr int kMaxPressMs = 10000;
constexpr int kPsHoldMs = 1500;
constexpr std::size_t kMaxRequestSize = 4096;

constexpr std::pair<QuickAction, std::string_view> kActionNames[] = {
    {QuickAction::PsButton, "ps_button"},
    {QuickAction::PsButtonHold, "ps_button_hold"},
    {QuickAction::Sleep, "sleep"},
    {QuickAction::Screenshot, "screenshot"},
    {QuickAction::ToggleMicrophone, "toggle_microphone"},
    {QuickAction::NextProfile, "next_profile"},
    {QuickAction::ToggleFrameGeneration, "toggle_frame_generation"},
    {QuickAction::Disconnect, "disconnect"},
};

struct RequestError {
    std::string code;
    std::string message;
};

/// Builds messages like: "ttl_ms" is out of range
std::string fieldMessage(std::string_view field, std::string_view problem) {
    std::string message;
    message.reserve(field.size() + problem.size() + 3);
    message += '"';
    message += field;
    message += "\" ";
    message += problem;
    return message;
}

json errorResponse(std::string_view code, std::string_view message) {
    return json{{"ok", false}, {"error", code}, {"message", message}};
}

std::uint32_t parseButtons(const json& value) {
    if (!value.is_array()) throw RequestError{"invalid_argument", "\"buttons\" must be an array of button names"};
    std::uint32_t buttons = 0;
    for (const json& entry : value) {
        if (!entry.is_string()) throw RequestError{"invalid_argument", "button names must be strings"};
        const auto button = parseButton(entry.get<std::string>());
        if (!button) throw RequestError{"invalid_argument", fieldMessage(entry.get<std::string>(), "is not a known button")};
        buttons |= mask(*button);
    }
    return buttons;
}

double parseUnit(const json& value, std::string_view name, double min, double max) {
    if (!value.is_number()) throw RequestError{"invalid_argument", fieldMessage(name, "must be a number")};
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < min || number > max) {
        throw RequestError{"invalid_argument", fieldMessage(name, "is out of range")};
    }
    return number;
}

void parseStick(const json& value, std::string_view name, std::int16_t& x, std::int16_t& y) {
    if (!value.is_array() || value.size() != 2) {
        throw RequestError{"invalid_argument", fieldMessage(name, "must be [x, y]")};
    }
    x = static_cast<std::int16_t>(std::lround(parseUnit(value[0], name, -1.0, 1.0) * 32767.0));
    y = static_cast<std::int16_t>(std::lround(parseUnit(value[1], name, -1.0, 1.0) * 32767.0));
}

int parseDuration(const json& request, const char* key, int fallback, int max) {
    if (!request.contains(key)) return fallback;
    const json& value = request[key];
    if (!value.is_number_integer()) throw RequestError{"invalid_argument", fieldMessage(key, "must be an integer")};
    const auto ms = value.get<std::int64_t>();
    if (ms < 1 || ms > max) throw RequestError{"invalid_argument", fieldMessage(key, "is out of range")};
    return static_cast<int>(ms);
}

} // namespace

std::string_view quickActionName(QuickAction action) {
    for (const auto& [value, name] : kActionNames) {
        if (value == action) return name;
    }
    return "unknown";
}

std::optional<QuickAction> parseQuickAction(std::string_view name) {
    for (const auto& [value, known] : kActionNames) {
        if (known == name) return value;
    }
    return std::nullopt;
}

RemoteCommandProcessor::RemoteCommandProcessor(VirtualController& controller, RemoteControlHooks hooks)
    : controller_(controller), hooks_(std::move(hooks)) {}

void RemoteCommandProcessor::setToken(std::string token) {
    std::lock_guard lock(mutex_);
    token_ = std::move(token);
}

std::string RemoteCommandProcessor::handle(std::string_view request, Clock::time_point now) {
    json response;
    json id;
    try {
        if (request.size() > kMaxRequestSize) throw RequestError{"invalid_json", "request is too large"};
        const json parsed = json::parse(request, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            throw RequestError{"invalid_json", "request must be one JSON object"};
        }
        if (parsed.contains("id")) id = parsed["id"];

        std::string token;
        {
            std::lock_guard lock(mutex_);
            token = token_;
        }
        if (!token.empty() && (!parsed.contains("token") || !parsed["token"].is_string() ||
                               parsed["token"].get<std::string>() != token)) {
            throw RequestError{"unauthorized", "missing or wrong token"};
        }
        response = dispatch(parsed, now);
    } catch (const RequestError& error) {
        response = errorResponse(error.code, error.message);
    } catch (const json::exception& error) {
        response = errorResponse("invalid_argument", error.what());
    }
    if (!id.is_null()) response["id"] = id;
    return response.dump();
}

json RemoteCommandProcessor::dispatch(const json& request, Clock::time_point now) {
    if (!request.contains("cmd") || !request["cmd"].is_string()) {
        throw RequestError{"invalid_argument", "\"cmd\" is missing"};
    }
    const std::string cmd = request["cmd"].get<std::string>();

    if (cmd == "ping") {
        return json{{"ok", true}, {"app", "VeloPlay"}, {"version", VELOPLAY_VERSION}, {"protocol", kRemoteControlProtocolVersion}};
    }

    if (cmd == "status") {
        json response{{"ok", true}};
        response["status"] = hooks_.status ? hooks_.status() : json::object();
        return response;
    }

    if (cmd == "set_state") {
        ControllerState state;
        if (request.contains("buttons")) state.buttons = parseButtons(request["buttons"]);
        if (request.contains("left_stick")) parseStick(request["left_stick"], "left_stick", state.leftX, state.leftY);
        if (request.contains("right_stick")) parseStick(request["right_stick"], "right_stick", state.rightX, state.rightY);
        if (request.contains("l2")) state.l2 = static_cast<std::uint8_t>(std::lround(parseUnit(request["l2"], "l2", 0.0, 1.0) * 255.0));
        if (request.contains("r2")) state.r2 = static_cast<std::uint8_t>(std::lround(parseUnit(request["r2"], "r2", 0.0, 1.0) * 255.0));
        const int ttl = parseDuration(request, "ttl_ms", kDefaultStateTtlMs, kMaxStateTtlMs);
        controller_.setState(state, milliseconds(ttl), now);
        return json{{"ok", true}};
    }

    if (cmd == "press") {
        if (!request.contains("buttons")) throw RequestError{"invalid_argument", "\"buttons\" is missing"};
        const std::uint32_t buttons = parseButtons(request["buttons"]);
        const int duration = parseDuration(request, "duration_ms", kDefaultPressMs, kMaxPressMs);
        controller_.press(buttons, milliseconds(duration), now);
        return json{{"ok", true}};
    }

    if (cmd == "release_all") {
        controller_.releaseAll();
        return json{{"ok", true}};
    }

    if (cmd == "action") {
        if (!request.contains("name") || !request["name"].is_string()) {
            throw RequestError{"invalid_argument", "\"name\" is missing"};
        }
        const auto action = parseQuickAction(request["name"].get<std::string>());
        if (!action) throw RequestError{"invalid_argument", "unknown action"};

        // The PS button actions are plain button presses and need no session logic.
        if (*action == QuickAction::PsButton) {
            controller_.press(mask(Button::Ps), milliseconds(kDefaultPressMs), now);
            return json{{"ok", true}};
        }
        if (*action == QuickAction::PsButtonHold) {
            controller_.press(mask(Button::Ps), milliseconds(kPsHoldMs), now);
            return json{{"ok", true}};
        }

        const std::string error = hooks_.action ? hooks_.action(*action) : std::string("not_available");
        if (!error.empty()) return errorResponse(error, "the action could not run");
        return json{{"ok", true}};
    }

    throw RequestError{"unknown_command", fieldMessage(cmd, "is not a known command")};
}

RemoteControlServer::RemoteControlServer(RemoteCommandProcessor& processor) : processor_(processor) {}

RemoteControlServer::~RemoteControlServer() { stop(); }

Result<void> RemoteControlServer::start(std::uint16_t port) {
    stop();
    auto socket = net::UdpSocket::open({{net::kLoopbackAddress, port}, false});
    if (!socket) return socket.error();
    socket_ = std::move(*socket);
    port_ = socket_.localEndpoint().port;
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    spdlog::info("Remote control API listening on 127.0.0.1:{}", port_);
    return {};
}

void RemoteControlServer::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
    socket_.close();
    port_ = 0;
}

void RemoteControlServer::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        auto datagram = socket_.receive(std::chrono::milliseconds(100), kMaxRequestSize + 1);
        if (!datagram) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (!datagram->has_value()) continue;
        const auto& received = **datagram;

        // The socket is bound to the loopback interface, so this cannot be
        // false on a sane network stack. Checked anyway, it is one line.
        if (!net::isLoopback(received.from.address)) continue;

        const std::string response = processor_.handle(asText(received.data), VirtualController::Clock::now());
        const auto sent = socket_.sendTo(asBytes(response), received.from);
        if (!sent) spdlog::debug("Remote control answer failed: {}", sent.error().message);
    }
}

} // namespace veloplay::input
