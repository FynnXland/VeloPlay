#include <gtest/gtest.h>

#include <vector>

#include "app/AppState.hpp"

using namespace veloplay;

TEST(AppState, StartsIdle) {
    AppStateMachine machine;
    EXPECT_EQ(machine.state(), AppState::Idle);
}

TEST(AppState, PlainConnectAndDisconnect) {
    AppStateMachine machine;
    EXPECT_TRUE(machine.handle(AppEvent::ConnectRequested));
    EXPECT_EQ(machine.state(), AppState::Connecting);
    EXPECT_TRUE(machine.handle(AppEvent::SessionEstablished));
    EXPECT_EQ(machine.state(), AppState::Streaming);
    EXPECT_TRUE(machine.handle(AppEvent::DisconnectRequested));
    EXPECT_EQ(machine.state(), AppState::Disconnecting);
    EXPECT_TRUE(machine.handle(AppEvent::Disconnected));
    EXPECT_EQ(machine.state(), AppState::Idle);
}

TEST(AppState, WakeOnlyReturnsToIdle) {
    AppStateMachine machine;
    EXPECT_TRUE(machine.handle(AppEvent::WakeRequested));
    EXPECT_EQ(machine.state(), AppState::Waking);
    EXPECT_FALSE(machine.connectAfterWake());
    EXPECT_TRUE(machine.handle(AppEvent::HostReady));
    EXPECT_EQ(machine.state(), AppState::Idle);
}

TEST(AppState, WakeAndConnectContinuesIntoConnecting) {
    AppStateMachine machine;
    EXPECT_TRUE(machine.wakeAndConnect());
    EXPECT_TRUE(machine.connectAfterWake());
    EXPECT_TRUE(machine.handle(AppEvent::HostReady));
    EXPECT_EQ(machine.state(), AppState::Connecting);
    EXPECT_FALSE(machine.connectAfterWake()); // one-shot

    EXPECT_FALSE(machine.wakeAndConnect()); // not from Connecting
}

TEST(AppState, WakeTimeoutNeedsAcknowledgement) {
    AppStateMachine machine;
    machine.wakeAndConnect();
    EXPECT_TRUE(machine.handle(AppEvent::WakeTimedOut));
    EXPECT_EQ(machine.state(), AppState::Error);
    EXPECT_FALSE(machine.connectAfterWake());
    EXPECT_FALSE(machine.handle(AppEvent::ConnectRequested));
    EXPECT_TRUE(machine.handle(AppEvent::ErrorAcknowledged));
    EXPECT_EQ(machine.state(), AppState::Idle);
}

TEST(AppState, StreamLossGoesThroughReconnect) {
    AppStateMachine machine;
    machine.handle(AppEvent::ConnectRequested);
    machine.handle(AppEvent::SessionEstablished);
    EXPECT_TRUE(machine.handle(AppEvent::ConnectionLost));
    EXPECT_EQ(machine.state(), AppState::Reconnecting);
    EXPECT_TRUE(machine.handle(AppEvent::ReconnectSucceeded));
    EXPECT_EQ(machine.state(), AppState::Streaming);

    machine.handle(AppEvent::ConnectionLost);
    EXPECT_TRUE(machine.handle(AppEvent::ReconnectFailed));
    EXPECT_EQ(machine.state(), AppState::Error);
}

TEST(AppState, InvalidEventsChangeNothing) {
    AppStateMachine machine;
    int notifications = 0;
    machine.setObserver([&](AppState, AppState, AppEvent) { ++notifications; });

    EXPECT_FALSE(machine.handle(AppEvent::SessionEstablished));
    EXPECT_FALSE(machine.handle(AppEvent::ConnectionLost));
    EXPECT_FALSE(machine.handle(AppEvent::Disconnected));
    EXPECT_EQ(machine.state(), AppState::Idle);
    EXPECT_EQ(notifications, 0);
}

TEST(AppState, ObserverSeesEveryTransition) {
    AppStateMachine machine;
    std::vector<std::pair<AppState, AppState>> seen;
    machine.setObserver([&](AppState from, AppState to, AppEvent) { seen.emplace_back(from, to); });
    machine.handle(AppEvent::ConnectRequested);
    machine.handle(AppEvent::ConnectionFailed);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], std::make_pair(AppState::Idle, AppState::Connecting));
    EXPECT_EQ(seen[1], std::make_pair(AppState::Connecting, AppState::Error));
}

// Every state must have a way out, otherwise the UI could get stuck.
TEST(AppState, NoStateIsADeadEnd) {
    const AppState states[] = {AppState::Idle,         AppState::Waking,        AppState::Connecting, AppState::Streaming,
                               AppState::Reconnecting, AppState::Disconnecting, AppState::Error};
    const AppEvent events[] = {AppEvent::WakeRequested,      AppEvent::ConnectRequested,   AppEvent::HostReady,
                               AppEvent::WakeTimedOut,       AppEvent::SessionEstablished, AppEvent::ConnectionFailed,
                               AppEvent::ConnectionLost,     AppEvent::ReconnectSucceeded, AppEvent::ReconnectFailed,
                               AppEvent::DisconnectRequested, AppEvent::Disconnected,      AppEvent::ErrorAcknowledged};
    for (AppState state : states) {
        bool hasExit = false;
        for (AppEvent event : events) {
            const auto next = nextState(state, event, false);
            if (next && *next != state) hasExit = true;
        }
        EXPECT_TRUE(hasExit) << toString(state);
        EXPECT_NE(toString(state), "unknown");
    }
    for (AppEvent event : events) EXPECT_NE(toString(event), "unknown");
}
