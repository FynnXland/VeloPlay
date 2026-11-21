#pragma once

#include <functional>
#include <optional>
#include <string_view>

namespace veloplay {

enum class AppState {
    Idle,          // dashboard, nothing running
    Waking,        // wake-up sent, waiting for the console to boot
    Connecting,    // session handshake in progress
    Streaming,
    Reconnecting,  // stream lost, trying to get it back
    Disconnecting, // orderly shutdown of a session
    Error,         // something failed; waits for the user to acknowledge
};

enum class AppEvent {
    WakeRequested,
    ConnectRequested,
    HostReady,          // console answers discovery with "ready"
    WakeTimedOut,
    SessionEstablished,
    ConnectionFailed,
    ConnectionLost,
    ReconnectSucceeded,
    ReconnectFailed,
    DisconnectRequested,
    Disconnected,
    ErrorAcknowledged,
};

std::string_view toString(AppState state);
std::string_view toString(AppEvent event);

/// The state `event` leads to from `state`, or nullopt if the event makes no
/// sense there. `connectAfterWake` decides where a finished wake-up goes.
std::optional<AppState> nextState(AppState state, AppEvent event, bool connectAfterWake);

class AppStateMachine {
public:
    using Observer = std::function<void(AppState from, AppState to, AppEvent event)>;

    [[nodiscard]] AppState state() const { return state_; }

    /// True while a wake-up should continue into a connection.
    [[nodiscard]] bool connectAfterWake() const { return connectAfterWake_; }

    void setObserver(Observer observer) { observer_ = std::move(observer); }

    /// Applies the event. Returns false and changes nothing if it is not valid
    /// in the current state.
    bool handle(AppEvent event);

    /// Wake the console, then connect once it is ready.
    bool wakeAndConnect();

private:
    AppState state_ = AppState::Idle;
    bool connectAfterWake_ = false;
    Observer observer_;
};

} // namespace veloplay
