#include "app/AppState.hpp"

namespace veloplay {

std::string_view toString(AppState state) {
    switch (state) {
        case AppState::Idle: return "idle";
        case AppState::Waking: return "waking";
        case AppState::Connecting: return "connecting";
        case AppState::Streaming: return "streaming";
        case AppState::Reconnecting: return "reconnecting";
        case AppState::Disconnecting: return "disconnecting";
        case AppState::Error: return "error";
    }
    return "unknown";
}

std::string_view toString(AppEvent event) {
    switch (event) {
        case AppEvent::WakeRequested: return "wake_requested";
        case AppEvent::ConnectRequested: return "connect_requested";
        case AppEvent::HostReady: return "host_ready";
        case AppEvent::WakeTimedOut: return "wake_timed_out";
        case AppEvent::SessionEstablished: return "session_established";
        case AppEvent::ConnectionFailed: return "connection_failed";
        case AppEvent::ConnectionLost: return "connection_lost";
        case AppEvent::ReconnectSucceeded: return "reconnect_succeeded";
        case AppEvent::ReconnectFailed: return "reconnect_failed";
        case AppEvent::DisconnectRequested: return "disconnect_requested";
        case AppEvent::Disconnected: return "disconnected";
        case AppEvent::ErrorAcknowledged: return "error_acknowledged";
    }
    return "unknown";
}

std::optional<AppState> nextState(AppState state, AppEvent event, bool connectAfterWake) {
    switch (state) {
        case AppState::Idle:
            if (event == AppEvent::WakeRequested) return AppState::Waking;
            if (event == AppEvent::ConnectRequested) return AppState::Connecting;
            break;
        case AppState::Waking:
            if (event == AppEvent::HostReady) return connectAfterWake ? AppState::Connecting : AppState::Idle;
            if (event == AppEvent::WakeTimedOut) return AppState::Error;
            if (event == AppEvent::DisconnectRequested) return AppState::Idle;
            break;
        case AppState::Connecting:
            if (event == AppEvent::SessionEstablished) return AppState::Streaming;
            if (event == AppEvent::ConnectionFailed) return AppState::Error;
            if (event == AppEvent::DisconnectRequested) return AppState::Disconnecting;
            break;
        case AppState::Streaming:
            if (event == AppEvent::ConnectionLost) return AppState::Reconnecting;
            if (event == AppEvent::DisconnectRequested) return AppState::Disconnecting;
            break;
        case AppState::Reconnecting:
            if (event == AppEvent::ReconnectSucceeded) return AppState::Streaming;
            if (event == AppEvent::ReconnectFailed) return AppState::Error;
            if (event == AppEvent::DisconnectRequested) return AppState::Disconnecting;
            break;
        case AppState::Disconnecting:
            if (event == AppEvent::Disconnected) return AppState::Idle;
            break;
        case AppState::Error:
            if (event == AppEvent::ErrorAcknowledged) return AppState::Idle;
            break;
    }
    return std::nullopt;
}

bool AppStateMachine::handle(AppEvent event) {
    const auto next = nextState(state_, event, connectAfterWake_);
    if (!next) return false;

    const AppState previous = state_;
    state_ = *next;
    if (state_ != AppState::Waking) connectAfterWake_ = false;
    if (observer_) observer_(previous, state_, event);
    return true;
}

bool AppStateMachine::wakeAndConnect() {
    if (state_ != AppState::Idle) return false;
    connectAfterWake_ = true;
    if (handle(AppEvent::WakeRequested)) return true;
    connectAfterWake_ = false;
    return false;
}

} // namespace veloplay
