#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "net/Socket.hpp"

using namespace veloplay;
using namespace veloplay::net;
using namespace std::chrono_literals;

TEST(Ipv4, ParsesDottedQuad) {
    EXPECT_EQ(parseIpv4("192.168.1.20"), 0xc0a80114u);
    EXPECT_EQ(parseIpv4("0.0.0.0"), 0u);
    EXPECT_EQ(parseIpv4("255.255.255.255"), 0xffffffffu);
    EXPECT_EQ(formatIpv4(0xc0a80114), "192.168.1.20");
    EXPECT_EQ(toString(Endpoint{0x7f000001, 9302}), "127.0.0.1:9302");
}

TEST(Ipv4, RejectsEverythingElse) {
    for (const char* text : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.", ".1.2.3", "1..2.3", "a.b.c.d",
                             "1.2.3.4 ", " 1.2.3.4", "ps5.local", "1.2.3.-4", "0001.2.3.4"}) {
        EXPECT_FALSE(parseIpv4(text)) << text;
    }
}

TEST(Ipv4, LoopbackRange) {
    EXPECT_TRUE(isLoopback(0x7f000001));
    EXPECT_TRUE(isLoopback(0x7f123456));
    EXPECT_FALSE(isLoopback(0xc0a80114));
}

TEST(Interfaces, BroadcastListAlwaysContainsLimitedBroadcast) {
    const auto addresses = broadcastAddresses();
    EXPECT_NE(std::find(addresses.begin(), addresses.end(), kBroadcastAddress), addresses.end());
}

TEST(UdpSocket, SendsAndReceivesOnLoopback) {
    auto a = UdpSocket::open({{kLoopbackAddress, 0}, false});
    auto b = UdpSocket::open({{kLoopbackAddress, 0}, false});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_NE(b->localEndpoint().port, 0);

    ASSERT_TRUE(a->sendTo(asBytes("ping"), b->localEndpoint()));
    const auto received = b->receive(1000ms);
    ASSERT_TRUE(received);
    ASSERT_TRUE(received->has_value());
    EXPECT_EQ(asText((*received)->data), "ping");
    EXPECT_EQ((*received)->from, a->localEndpoint());
}

TEST(UdpSocket, ReceiveTimesOutQuietly) {
    auto socket = UdpSocket::open({{kLoopbackAddress, 0}, false});
    ASSERT_TRUE(socket);
    const auto received = socket->receive(30ms);
    ASSERT_TRUE(received);
    EXPECT_FALSE(received->has_value());
}

TEST(UdpSocket, SurvivesSendingToClosedPort) {
    // On Windows an ICMP "port unreachable" must not poison the next receive.
    auto closed = UdpSocket::open({{kLoopbackAddress, 0}, false});
    ASSERT_TRUE(closed);
    const Endpoint dead = closed->localEndpoint();
    closed->close();

    auto socket = UdpSocket::open({{kLoopbackAddress, 0}, false});
    ASSERT_TRUE(socket);
    (void)socket->sendTo(asBytes("x"), dead);
    std::this_thread::sleep_for(20ms);

    auto peer = UdpSocket::open({{kLoopbackAddress, 0}, false});
    ASSERT_TRUE(peer);
    ASSERT_TRUE(peer->sendTo(asBytes("still alive"), socket->localEndpoint()));

    // Linux reports the ICMP error once on the next call; the datagram that
    // follows must still arrive.
    std::optional<Datagram> datagram;
    for (int attempt = 0; attempt < 3 && !datagram; ++attempt) {
        auto received = socket->receive(500ms);
        if (received && received->has_value()) datagram = std::move(**received);
    }
    ASSERT_TRUE(datagram);
    EXPECT_EQ(asText(datagram->data), "still alive");
}

TEST(TcpSocket, ExchangesDataWithListener) {
    auto listener = TcpListener::listen({kLoopbackAddress, 0});
    ASSERT_TRUE(listener);
    const Endpoint endpoint = listener->localEndpoint();

    std::jthread server([&] {
        auto accepted = listener->accept(2000ms);
        if (!accepted || !accepted->has_value()) return;
        TcpSocket client = std::move(**accepted);
        std::uint8_t buffer[64];
        const auto received = client.receiveSome(buffer, 2000ms);
        if (received && *received > 0) (void)client.sendAll({buffer, *received}, 2000ms);
    });

    auto socket = TcpSocket::connect(endpoint, 2000ms);
    ASSERT_TRUE(socket) << socket.error().message;
    ASSERT_TRUE(socket->sendAll(asBytes("echo me"), 2000ms));

    std::uint8_t buffer[64];
    const auto received = socket->receiveSome(buffer, 2000ms);
    ASSERT_TRUE(received);
    EXPECT_EQ(asText({buffer, *received}), "echo me");

    server.join();
    const auto closed = socket->receiveSome(buffer, 2000ms);
    ASSERT_TRUE(closed);
    EXPECT_EQ(*closed, 0u); // peer closed
}

TEST(TcpSocket, ConnectToClosedPortFails) {
    auto listener = TcpListener::listen({kLoopbackAddress, 0});
    ASSERT_TRUE(listener);
    const Endpoint endpoint = listener->localEndpoint();
    listener->close();

    const auto socket = TcpSocket::connect(endpoint, 1000ms);
    EXPECT_FALSE(socket);
}

TEST(TcpSocket, ReceiveTimesOut) {
    auto listener = TcpListener::listen({kLoopbackAddress, 0});
    ASSERT_TRUE(listener);
    auto socket = TcpSocket::connect(listener->localEndpoint(), 1000ms);
    ASSERT_TRUE(socket);
    std::uint8_t buffer[8];
    const auto received = socket->receiveSome(buffer, 50ms);
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code, ErrorCode::Timeout);
}
