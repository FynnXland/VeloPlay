#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "input/RemoteControl.hpp"

using namespace veloplay;
using namespace veloplay::input;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

struct Fixture {
    VirtualController controller;
    std::vector<QuickAction> actions;
    std::string actionError;
    RemoteCommandProcessor processor{controller, RemoteControlHooks{
                                                     [] { return json{{"state", "idle"}}; },
                                                     [this](QuickAction action) {
                                                         actions.push_back(action);
                                                         return actionError;
                                                     },
                                                 }};
    VirtualController::Clock::time_point t0{};

    json send(const json& request) { return json::parse(processor.handle(request.dump(), t0)); }
};

} // namespace

TEST(RemoteControl, PingReportsVersion) {
    Fixture f;
    const json response = f.send({{"cmd", "ping"}, {"id", 42}});
    EXPECT_TRUE(response["ok"].get<bool>());
    EXPECT_EQ(response["app"], "VeloPlay");
    EXPECT_EQ(response["protocol"], kRemoteControlProtocolVersion);
    EXPECT_EQ(response["id"], 42); // echoed so scripts can match answers
}

TEST(RemoteControl, StatusComesFromTheApplication) {
    Fixture f;
    const json response = f.send({{"cmd", "status"}});
    EXPECT_TRUE(response["ok"].get<bool>());
    EXPECT_EQ(response["status"]["state"], "idle");
}

TEST(RemoteControl, SetStateMapsRangesAndExpires) {
    Fixture f;
    const json response = f.send({{"cmd", "set_state"},
                                  {"buttons", {"cross", "l1"}},
                                  {"left_stick", {1.0, -0.5}},
                                  {"r2", 1.0},
                                  {"ttl_ms", 400}});
    ASSERT_TRUE(response["ok"].get<bool>()) << response.dump();

    const ControllerState state = f.controller.state(f.t0 + 399ms);
    EXPECT_EQ(state.buttons, mask(Button::Cross) | mask(Button::L1));
    EXPECT_EQ(state.leftX, 32767);
    EXPECT_EQ(state.leftY, -16384);
    EXPECT_EQ(state.r2, 255);
    EXPECT_EQ(state.l2, 0);
    EXPECT_EQ(f.controller.state(f.t0 + 400ms), ControllerState{});
}

TEST(RemoteControl, SetStateUsesShortDefaultLifetime) {
    Fixture f;
    f.send({{"cmd", "set_state"}, {"buttons", {"circle"}}});
    EXPECT_TRUE(f.controller.state(f.t0 + 200ms).pressed(Button::Circle));
    EXPECT_FALSE(f.controller.state(f.t0 + 300ms).pressed(Button::Circle));
}

TEST(RemoteControl, PressHoldsButtonsForTheGivenTime) {
    Fixture f;
    ASSERT_TRUE(f.send({{"cmd", "press"}, {"buttons", {"options"}}, {"duration_ms", 150}})["ok"].get<bool>());
    EXPECT_TRUE(f.controller.state(f.t0 + 149ms).pressed(Button::Options));
    EXPECT_FALSE(f.controller.state(f.t0 + 150ms).pressed(Button::Options));
}

TEST(RemoteControl, ReleaseAll) {
    Fixture f;
    f.send({{"cmd", "press"}, {"buttons", {"square"}}, {"duration_ms", 5000}});
    ASSERT_TRUE(f.send({{"cmd", "release_all"}})["ok"].get<bool>());
    EXPECT_EQ(f.controller.state(f.t0 + 1ms), ControllerState{});
}

TEST(RemoteControl, PsButtonActionsArePlainPresses) {
    Fixture f;
    ASSERT_TRUE(f.send({{"cmd", "action"}, {"name", "ps_button"}})["ok"].get<bool>());
    EXPECT_TRUE(f.controller.state(f.t0 + 50ms).pressed(Button::Ps));
    EXPECT_FALSE(f.controller.state(f.t0 + 200ms).pressed(Button::Ps));

    ASSERT_TRUE(f.send({{"cmd", "action"}, {"name", "ps_button_hold"}})["ok"].get<bool>());
    EXPECT_TRUE(f.controller.state(f.t0 + 1400ms).pressed(Button::Ps));
    EXPECT_TRUE(f.actions.empty()); // handled without the application
}

TEST(RemoteControl, OtherActionsGoToTheApplication) {
    Fixture f;
    ASSERT_TRUE(f.send({{"cmd", "action"}, {"name", "next_profile"}})["ok"].get<bool>());
    ASSERT_EQ(f.actions.size(), 1u);
    EXPECT_EQ(f.actions[0], QuickAction::NextProfile);

    f.actionError = "no_active_session";
    const json response = f.send({{"cmd", "action"}, {"name", "sleep"}});
    EXPECT_FALSE(response["ok"].get<bool>());
    EXPECT_EQ(response["error"], "no_active_session");
}

TEST(RemoteControl, ActionNamesRoundTrip) {
    for (QuickAction action : {QuickAction::PsButton, QuickAction::PsButtonHold, QuickAction::Sleep,
                               QuickAction::Screenshot, QuickAction::ToggleMicrophone, QuickAction::NextProfile,
                               QuickAction::ToggleFrameGeneration, QuickAction::Disconnect}) {
        EXPECT_EQ(parseQuickAction(quickActionName(action)), action);
    }
    EXPECT_FALSE(parseQuickAction("explode"));
}

TEST(RemoteControl, RejectsBadRequestsWithoutSideEffects) {
    Fixture f;
    const auto errorOf = [&](std::string_view raw) {
        return json::parse(f.processor.handle(raw, f.t0)).value("error", "");
    };
    EXPECT_EQ(errorOf("not json"), "invalid_json");
    EXPECT_EQ(errorOf("[1,2,3]"), "invalid_json");
    EXPECT_EQ(errorOf(std::string(5000, ' ')), "invalid_json");
    EXPECT_EQ(errorOf(R"({"no_cmd":1})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"fly"})"), "unknown_command");
    EXPECT_EQ(errorOf(R"({"cmd":"press"})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"press","buttons":["turbo"]})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"press","buttons":"cross"})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"press","buttons":["cross"],"duration_ms":0})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"press","buttons":["cross"],"duration_ms":999999})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"set_state","left_stick":[2.0,0]})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"set_state","left_stick":[0]})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"set_state","l2":"full"})"), "invalid_argument");
    EXPECT_EQ(errorOf(R"({"cmd":"action","name":"explode"})"), "invalid_argument");
    EXPECT_EQ(f.controller.state(f.t0 + 1ms), ControllerState{});
}

TEST(RemoteControl, TokenIsEnforcedWhenSet) {
    Fixture f;
    f.processor.setToken("s3cret");
    EXPECT_EQ(f.send({{"cmd", "ping"}})["error"], "unauthorized");
    EXPECT_EQ(f.send({{"cmd", "ping"}, {"token", "wrong"}})["error"], "unauthorized");
    EXPECT_EQ(f.send({{"cmd", "press"}, {"buttons", {"ps"}}, {"token", 5}})["error"], "unauthorized");
    EXPECT_EQ(f.controller.state(f.t0 + 1ms), ControllerState{});
    EXPECT_TRUE(f.send({{"cmd", "ping"}, {"token", "s3cret"}})["ok"].get<bool>());
}

TEST(RemoteControlServer, AnswersDatagramsOnLoopback) {
    Fixture f;
    RemoteControlServer server(f.processor);
    ASSERT_TRUE(server.start(0));
    ASSERT_NE(server.port(), 0);

    auto client = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
    ASSERT_TRUE(client);
    const std::string request = R"({"cmd":"press","buttons":["triangle"],"duration_ms":5000,"id":"a"})";
    ASSERT_TRUE(client->sendTo(asBytes(request), {net::kLoopbackAddress, server.port()}));

    const auto answer = client->receive(2000ms);
    ASSERT_TRUE(answer);
    ASSERT_TRUE(answer->has_value());
    const json response = json::parse(asText((*answer)->data));
    EXPECT_TRUE(response["ok"].get<bool>());
    EXPECT_EQ(response["id"], "a");
    EXPECT_TRUE(f.controller.state(VirtualController::Clock::now()).pressed(Button::Triangle));

    server.stop();
    EXPECT_FALSE(server.running());
}

TEST(RemoteControlServer, ReportsPortInUse) {
    Fixture f;
    RemoteControlServer first(f.processor);
    ASSERT_TRUE(first.start(0));
    RemoteControlServer second(f.processor);
    const auto started = second.start(first.port());
    EXPECT_FALSE(started);
}
