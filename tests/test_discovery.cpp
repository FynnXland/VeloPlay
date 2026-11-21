#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <thread>

#include "MockConsole.hpp"
#include "protocol/Discovery.hpp"
#include "protocol/DiscoveryService.hpp"

using namespace veloplay;
using namespace veloplay::protocol;
using namespace std::chrono_literals;

TEST(DiscoveryPackets, SearchRequestMatchesWireFormat) {
    const std::string text = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
    // The terminating NUL is sent as part of the datagram.
    EXPECT_EQ(buildSearchRequest(), text + '\0');
}

TEST(DiscoveryPackets, WakeupRequestMatchesWireFormat) {
    const std::string body = "WAKEUP * HTTP/1.1\n"
                             "client-type:vr\n"
                             "auth-type:R\n"
                             "model:w\n"
                             "app-type:r\n"
                             "user-credential:457002842\n"
                             "device-discovery-protocol-version:00030010\n";
    EXPECT_EQ(buildWakeupRequest(457002842), body + '\0');
}

TEST(DiscoveryPackets, WakeCredentialIsRegistKeyReadAsHex) {
    EXPECT_EQ(wakeCredential("1b3d4f5a"), 0x1b3d4f5aull);
    EXPECT_EQ(wakeCredential("1B3D4F5A"), 0x1b3d4f5aull);
    EXPECT_EQ(wakeCredential("ffffffffffffffff"), 0xffffffffffffffffull);
    EXPECT_FALSE(wakeCredential(""));
    EXPECT_FALSE(wakeCredential("xyz"));
    EXPECT_FALSE(wakeCredential("1b3d4f5a "));
    EXPECT_FALSE(wakeCredential("11111111111111111")); // 17 digits
}

TEST(DiscoveryPackets, ParsesReadyConsole) {
    const auto response = parseDiscoveryResponse("HTTP/1.1 200 Ok\n"
                                                 "host-id:BC60A7123456\n"
                                                 "host-type:PS5\n"
                                                 "host-name:Wohnzimmer\n"
                                                 "host-request-port:997\n"
                                                 "device-discovery-protocol-version:00030010\n"
                                                 "system-version:09400004\n"
                                                 "running-app-name:Astro's Playroom\n"
                                                 "running-app-titleid:PPSA01325\n");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->state, HostState::Ready);
    EXPECT_EQ(response->hostId, "BC60A7123456");
    EXPECT_EQ(response->hostType, "PS5");
    EXPECT_EQ(response->hostName, "Wohnzimmer");
    EXPECT_EQ(response->requestPort, 997);
    EXPECT_EQ(response->systemVersion, "09400004");
    EXPECT_EQ(response->runningAppName, "Astro's Playroom");
    EXPECT_EQ(response->runningAppTitleId, "PPSA01325");
    EXPECT_TRUE(response->isPs5());
}

TEST(DiscoveryPackets, ParsesStandbyConsole) {
    const auto response = parseDiscoveryResponse("HTTP/1.1 620 Server Standby\n"
                                                 "host-id:BC60A7123456\n"
                                                 "host-type:PS5\n"
                                                 "host-name:Wohnzimmer\n"
                                                 "host-request-port:997\n"
                                                 "device-discovery-protocol-version:00030010\n");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->state, HostState::Standby);
    EXPECT_TRUE(response->runningAppName.empty());
}

TEST(DiscoveryPackets, UnknownStatusAndGarbage) {
    const auto odd = parseDiscoveryResponse("HTTP/1.1 503 Busy\nhost-id:AA\n");
    ASSERT_TRUE(odd);
    EXPECT_EQ(odd->state, HostState::Unknown);
    EXPECT_FALSE(parseDiscoveryResponse("hello"));
    EXPECT_FALSE(parseDiscoveryResponse(buildSearchRequest()));
}

TEST(HostTracker, KeepsLatestAnswerPerConsoleAndForgetsSilentOnes) {
    using Clock = HostTracker::Clock;
    HostTracker tracker(5s);
    const Clock::time_point t0{};

    DiscoveryResponse a;
    a.hostId = "AAAAAAAAAAAA";
    a.hostName = "B-Console";
    a.state = HostState::Standby;
    DiscoveryResponse b;
    b.hostId = "BBBBBBBBBBBB";
    b.hostName = "A-Console";
    b.state = HostState::Ready;

    tracker.update(a, 0xc0a80114, t0);
    tracker.update(b, 0xc0a80115, t0 + 1s);
    a.state = HostState::Ready;
    tracker.update(a, 0xc0a80199, t0 + 2s); // same console, new DHCP lease

    auto hosts = tracker.snapshot(t0 + 2s);
    ASSERT_EQ(hosts.size(), 2u);
    EXPECT_EQ(hosts[0].info.hostName, "A-Console"); // sorted by name
    EXPECT_EQ(hosts[1].info.state, HostState::Ready);
    EXPECT_EQ(hosts[1].address, 0xc0a80199u);

    hosts = tracker.snapshot(t0 + 6500ms); // b was last seen 5.5 s ago
    ASSERT_EQ(hosts.size(), 1u);
    EXPECT_EQ(hosts[0].info.hostId, "AAAAAAAAAAAA");

    tracker.prune(t0 + 60s);
    EXPECT_TRUE(tracker.snapshot(t0 + 60s).empty());
}

TEST(HostTracker, FallsBackToAddressWithoutHostId) {
    HostTracker tracker;
    DiscoveryResponse anonymous;
    anonymous.state = HostState::Ready;
    tracker.update(anonymous, 1, {});
    tracker.update(anonymous, 2, {});
    EXPECT_EQ(tracker.snapshot({}).size(), 2u);
}

namespace {

DiscoveryService::Config loopbackConfig(std::uint16_t consolePort) {
    DiscoveryService::Config config;
    config.consolePort = consolePort;
    config.localPortFirst = 0; // any free port
    config.searchInterval = 50ms;
    config.hostTimeout = 400ms;
    config.broadcast = false;
    return config;
}

} // namespace

TEST(DiscoveryService, FindsConsoleOverLoopback) {
    test::MockConsole console({});
    DiscoveryService service(loopbackConfig(console.discoveryPort()));
    service.setDirectTargets({net::kLoopbackAddress});
    ASSERT_TRUE(service.start());

    ASSERT_TRUE(test::waitFor([&] { return !service.hosts().empty(); }));
    const auto hosts = service.hosts();
    EXPECT_EQ(hosts[0].info.hostId, "001122AABBCC");
    EXPECT_EQ(hosts[0].info.hostName, "PS5-042");
    EXPECT_EQ(hosts[0].info.state, HostState::Ready);
    EXPECT_EQ(hosts[0].address, net::kLoopbackAddress);
}

TEST(DiscoveryService, WakesConsoleWithTheRightCredentialOnly) {
    test::MockConsole::Config consoleConfig;
    consoleConfig.standby = true;
    test::MockConsole console(consoleConfig);

    DiscoveryService service(loopbackConfig(console.discoveryPort()));
    service.setDirectTargets({net::kLoopbackAddress});
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(test::waitFor([&] {
        const auto hosts = service.hosts();
        return !hosts.empty() && hosts[0].info.state == HostState::Standby;
    }));

    ASSERT_TRUE(service.wake(net::kLoopbackAddress, 12345)); // wrong credential
    std::this_thread::sleep_for(150ms);
    EXPECT_TRUE(console.standby());

    ASSERT_TRUE(service.wake(net::kLoopbackAddress, *wakeCredential(test::MockConsole::registKeyAscii())));
    EXPECT_TRUE(test::waitFor([&] {
        const auto hosts = service.hosts();
        return !hosts.empty() && hosts[0].info.state == HostState::Ready;
    }));
    EXPECT_EQ(console.wakeupsReceived(), 1);
}

TEST(DiscoveryService, DropsConsoleThatStopsAnswering) {
    auto console = std::make_unique<test::MockConsole>(test::MockConsole::Config{});
    DiscoveryService service(loopbackConfig(console->discoveryPort()));
    service.setDirectTargets({net::kLoopbackAddress});
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(test::waitFor([&] { return !service.hosts().empty(); }));

    console.reset();
    EXPECT_TRUE(test::waitFor([&] { return service.hosts().empty(); }));
}

TEST(DiscoveryService, WakeWorksWithoutRunningSearch) {
    test::MockConsole::Config consoleConfig;
    consoleConfig.standby = true;
    test::MockConsole console(consoleConfig);

    DiscoveryService service(loopbackConfig(console.discoveryPort()));
    ASSERT_TRUE(service.wake(net::kLoopbackAddress, 0x1b3d4f5a));
    EXPECT_TRUE(test::waitFor([&] { return !console.standby(); }));
}

TEST(Sweep, ListsEveryOtherAddressOfSmallNetworks) {
    // A home network: 192.168.178.20 with mask 255.255.255.0.
    const net::LocalSubnet home{*net::parseIpv4("192.168.178.20"), 24};
    const auto addresses = sweepAddresses(std::array{home});
    ASSERT_EQ(addresses.size(), 253u); // 254 hosts without ourselves
    EXPECT_EQ(addresses.front(), *net::parseIpv4("192.168.178.1"));
    EXPECT_EQ(addresses.back(), *net::parseIpv4("192.168.178.254"));
    EXPECT_EQ(std::count(addresses.begin(), addresses.end(), home.address), 0);

    // Large networks are too many addresses to ask; point-to-point links have nobody else.
    EXPECT_TRUE(sweepAddresses(std::array{net::LocalSubnet{*net::parseIpv4("10.0.0.5"), 16}}).empty());
    EXPECT_TRUE(sweepAddresses(std::array{net::LocalSubnet{*net::parseIpv4("10.0.0.5"), 31}}).empty());
    EXPECT_TRUE(sweepAddresses(std::array{net::LocalSubnet{*net::parseIpv4("10.0.0.5"), 0}}).empty());

    // Two adapters in the same network do not double the work.
    const net::LocalSubnet second{*net::parseIpv4("192.168.178.21"), 24};
    EXPECT_EQ(sweepAddresses(std::array{home, second}).size(), 254u);

    const auto tiny = sweepAddresses(std::array{net::LocalSubnet{*net::parseIpv4("127.0.0.2"), 30}});
    ASSERT_EQ(tiny.size(), 1u);
    EXPECT_EQ(tiny[0], net::kLoopbackAddress);
}

TEST(Sweep, ReportsThisMachinesNetworks) {
    for (const net::LocalSubnet& subnet : net::localSubnets()) {
        EXPECT_LE(subnet.prefix, 32u);
        EXPECT_NE(subnet.address >> 24, 127u); // loopback is left out
    }
}

TEST(DiscoveryService, FindsAConsoleThatTheBroadcastDoesNotReach) {
    // No broadcast and no address given: only asking every address helps.
    test::MockConsole console({});
    auto config = loopbackConfig(console.discoveryPort());
    config.sweep = true;
    config.sweepAfter = 50ms;
    config.sweepInterval = 100ms;
    config.sweepSubnets = {{*net::parseIpv4("127.0.0.2"), 30}};
    config.hostTimeout = 400ms;
    DiscoveryService service(config);
    ASSERT_TRUE(service.start());

    ASSERT_TRUE(test::waitFor([&] { return !service.hosts().empty(); }));
    EXPECT_EQ(service.hosts()[0].address, net::kLoopbackAddress);

    // Once known, the console is asked directly and stays on the list well
    // beyond the time after which silent hosts are dropped.
    const int before = console.searchesReceived();
    std::this_thread::sleep_for(1000ms);
    EXPECT_FALSE(service.hosts().empty());
    EXPECT_GT(console.searchesReceived(), before);
}

TEST(DiscoveryService, DoesNotSweepUnlessAskedTo) {
    test::MockConsole console({});
    auto config = loopbackConfig(console.discoveryPort());
    config.sweepAfter = 20ms;
    config.sweepInterval = 50ms;
    DiscoveryService service(config); // broadcast off, no networks given
    ASSERT_TRUE(service.start());
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(service.hosts().empty());
    EXPECT_EQ(console.searchesReceived(), 0);
}
