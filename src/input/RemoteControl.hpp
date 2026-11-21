#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json_fwd.hpp>

#include "input/VirtualController.hpp"
#include "net/Socket.hpp"
#include "util/Result.hpp"

// Local remote control API. Scripts on the same PC send one JSON object per
// UDP datagram to 127.0.0.1 and get one JSON object back. The command set is
// documented in docs/remote_control.md.
//
// UDP is a deliberate choice: web pages can open WebSocket connections to
// localhost, but they cannot send UDP, so a browser tab cannot press buttons
// on the console.

namespace veloplay::input {

inline constexpr std::uint16_t kDefaultRemoteControlPort = 9876;
inline constexpr int kRemoteControlProtocolVersion = 1;

enum class QuickAction {
    PsButton,     // short press, opens the control center
    PsButtonHold, // long press
    Sleep,        // put the console into rest mode
    Screenshot,
    ToggleMicrophone,
    NextProfile,
    ToggleFrameGeneration,
    Disconnect,
};

std::string_view quickActionName(QuickAction action);
std::optional<QuickAction> parseQuickAction(std::string_view name);

struct RemoteControlHooks {
    /// Fills the answer to the "status" command. May be empty.
    std::function<nlohmann::json()> status;

    /// Runs an action. Returns an empty string on success, otherwise a short
    /// machine-readable error such as "no_active_session".
    std::function<std::string(QuickAction)> action;
};

/// Turns request JSON into changes on the VirtualController and builds the
/// answer. No sockets in here, which keeps it easy to test.
class RemoteCommandProcessor {
public:
    using Clock = VirtualController::Clock;

    RemoteCommandProcessor(VirtualController& controller, RemoteControlHooks hooks);

    /// If set, every request must carry the same value in "token".
    void setToken(std::string token);

    [[nodiscard]] std::string handle(std::string_view request, Clock::time_point now);

private:
    nlohmann::json dispatch(const nlohmann::json& request, Clock::time_point now);

    VirtualController& controller_;
    RemoteControlHooks hooks_;
    std::mutex mutex_;
    std::string token_;
};

/// UDP endpoint for RemoteCommandProcessor, bound to 127.0.0.1 only.
class RemoteControlServer {
public:
    explicit RemoteControlServer(RemoteCommandProcessor& processor);
    ~RemoteControlServer();

    RemoteControlServer(const RemoteControlServer&) = delete;
    RemoteControlServer& operator=(const RemoteControlServer&) = delete;

    /// Port 0 picks a free port; see port().
    Result<void> start(std::uint16_t port);
    void stop();

    [[nodiscard]] bool running() const { return thread_.joinable(); }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void run(std::stop_token stop);

    RemoteCommandProcessor& processor_;
    net::UdpSocket socket_;
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

} // namespace veloplay::input
