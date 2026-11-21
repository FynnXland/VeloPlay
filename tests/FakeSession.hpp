#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "session/ISession.hpp"

namespace veloplay::test {

class FakeSession;

/// Shared between a test and the sessions the AppController creates, so the
/// test can script what the "console" does and see what the client asked for.
struct FakeSessionHub {
    /// Events each new session reports right after start(), one entry per
    /// attempt. Attempts beyond the script report nothing on their own.
    std::deque<std::vector<session::SessionEvent>> scripts;
    bool failStart = false;
    bool failSleep = false;

    std::vector<session::SessionConfig> configs; // one per started session
    FakeSession* current = nullptr;
    int destroyed = 0;
    int stops = 0;
    int sleeps = 0;
    int controllerUpdates = 0;
    input::ControllerState lastInput{};
    std::vector<std::string> pins;
    session::SessionStats stats{};

    void emit(session::SessionEvent event);

    static session::SessionEvent connected() { return {}; }
    static session::SessionEvent quit(session::QuitReason reason, std::string detail = "scripted") {
        session::SessionEvent event;
        event.type = session::SessionEvent::Type::Quit;
        event.reason = reason;
        event.detail = std::move(detail);
        return event;
    }
    static session::SessionEvent loginPin(bool previousWrong) {
        session::SessionEvent event;
        event.type = session::SessionEvent::Type::LoginPinRequested;
        event.previousPinWrong = previousWrong;
        return event;
    }
    static session::SessionEvent rumble(std::uint8_t low, std::uint8_t high) {
        session::SessionEvent event;
        event.type = session::SessionEvent::Type::Rumble;
        event.rumbleLow = low;
        event.rumbleHigh = high;
        return event;
    }

    session::SessionFactory factory(std::shared_ptr<FakeSessionHub> self);
};

class FakeSession final : public session::ISession {
public:
    explicit FakeSession(std::shared_ptr<FakeSessionHub> hub) : hub_(std::move(hub)) {}
    ~FakeSession() override {
        ++hub_->destroyed;
        if (hub_->current == this) hub_->current = nullptr;
    }

    Result<void> start(const session::SessionConfig& config, session::IMediaSink*) override {
        if (hub_->failStart) return Error{ErrorCode::Io, "scripted start failure"};
        hub_->configs.push_back(config);
        hub_->current = this;
        if (!hub_->scripts.empty()) {
            events_ = std::move(hub_->scripts.front());
            hub_->scripts.pop_front();
        }
        return {};
    }

    void stop() override {
        ++hub_->stops;
        events_.push_back(FakeSessionHub::quit(session::QuitReason::Stopped));
    }

    std::vector<session::SessionEvent> pollEvents() override {
        std::vector<session::SessionEvent> out;
        out.swap(events_);
        return out;
    }

    void setControllerState(const input::ControllerState& state, session::Clock::time_point) override {
        ++hub_->controllerUpdates;
        hub_->lastInput = state;
    }

    Result<void> submitLoginPin(std::string_view digits) override {
        hub_->pins.emplace_back(digits);
        return {};
    }

    Result<void> sleepConsole() override {
        if (hub_->failSleep) return Error{ErrorCode::Network, "scripted sleep failure"};
        ++hub_->sleeps;
        return {};
    }

    session::SessionStats stats(session::Clock::time_point) override { return hub_->stats; }

    void push(session::SessionEvent event) { events_.push_back(std::move(event)); }

private:
    std::shared_ptr<FakeSessionHub> hub_;
    std::vector<session::SessionEvent> events_;
};

inline void FakeSessionHub::emit(session::SessionEvent event) {
    if (current) current->push(std::move(event));
}

inline session::SessionFactory FakeSessionHub::factory(std::shared_ptr<FakeSessionHub> self) {
    return [self] { return std::unique_ptr<session::ISession>(std::make_unique<FakeSession>(self)); };
}

} // namespace veloplay::test
