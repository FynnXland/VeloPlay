#include <gtest/gtest.h>

#include "app/ConsoleList.hpp"

using namespace veloplay;
using protocol::DiscoveredHost;
using protocol::HostState;

namespace {

RegisteredConsole registered(std::string name, std::string hostId, std::string address) {
    RegisteredConsole console;
    console.name = std::move(name);
    console.hostId = std::move(hostId);
    console.lastAddress = std::move(address);
    return console;
}

DiscoveredHost discovered(std::string name, std::string hostId, std::uint32_t address, HostState state) {
    DiscoveredHost host;
    host.info.hostName = std::move(name);
    host.info.hostId = std::move(hostId);
    host.info.hostType = "PS5";
    host.info.state = state;
    host.address = address;
    return host;
}

} // namespace

TEST(ConsoleList, PairedConsoleWithoutAnswerIsOffline) {
    const auto list = buildConsoleList({registered("Wohnzimmer", "AAAAAAAAAAAA", "192.168.1.20")}, {});
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].status, ConsoleStatus::Offline);
    EXPECT_TRUE(list[0].registered);
    EXPECT_EQ(list[0].address, "192.168.1.20"); // last known address is kept for wake-up
}

TEST(ConsoleList, DiscoveryUpdatesStatusAddressAndName) {
    auto host = discovered("Neues Zimmer", "aaaaaaaaaaaa", 0xc0a80163, HostState::Ready);
    host.info.runningAppName = "Astro Bot";
    host.info.runningAppTitleId = "PPSA21567";
    const auto list = buildConsoleList({registered("Wohnzimmer", "AAAAAAAAAAAA", "192.168.1.20")}, {host});
    ASSERT_EQ(list.size(), 1u); // matched despite different letter case
    EXPECT_EQ(list[0].status, ConsoleStatus::Ready);
    EXPECT_EQ(list[0].address, "192.168.1.99");
    EXPECT_EQ(list[0].name, "Neues Zimmer");
    EXPECT_EQ(list[0].runningApp, "Astro Bot");
    EXPECT_EQ(list[0].runningAppId, "PPSA21567");
}

TEST(ConsoleList, UnpairedConsolesFollowPairedOnes) {
    const auto list = buildConsoleList(
        {registered("Paired", "BBBBBBBBBBBB", "")},
        {discovered("Stranger", "CCCCCCCCCCCC", 0xc0a80105, HostState::Standby),
         discovered("Paired", "BBBBBBBBBBBB", 0xc0a80106, HostState::Standby)});
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "Paired");
    EXPECT_EQ(list[0].status, ConsoleStatus::Standby);
    EXPECT_EQ(list[1].name, "Stranger");
    EXPECT_FALSE(list[1].registered);
    EXPECT_FALSE(list[1].credentialsValid);
}

TEST(ConsoleList, NamelessConsoleGetsAFallbackName) {
    const auto list = buildConsoleList({}, {discovered("", "DDDDDDDDDDDD", 1, HostState::Ready)});
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "PS5");
}

TEST(ConsoleList, CarriesCredentialState) {
    auto console = registered("Kopie", "EEEEEEEEEEEE", "");
    console.credentialsValid = false;
    const auto list = buildConsoleList({console}, {});
    EXPECT_FALSE(list[0].credentialsValid);
}
