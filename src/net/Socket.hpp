#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/Bytes.hpp"
#include "util/Result.hpp"

// Thin RAII layer over BSD sockets / Winsock. IPv4 only: the console is
// reached over the local network, and its discovery protocol is broadcast based.

namespace veloplay::net {

using Milliseconds = std::chrono::milliseconds;

/// Addresses are kept in host byte order.
struct Endpoint {
    std::uint32_t address = 0;
    std::uint16_t port = 0;

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

inline constexpr std::uint32_t kAnyAddress = 0;
inline constexpr std::uint32_t kLoopbackAddress = 0x7f000001;
inline constexpr std::uint32_t kBroadcastAddress = 0xffffffff;

/// Strict dotted-quad parser ("192.168.1.20"). No host names.
std::optional<std::uint32_t> parseIpv4(std::string_view text);
std::string formatIpv4(std::uint32_t address);
std::string toString(const Endpoint& endpoint);

inline bool isLoopback(std::uint32_t address) { return (address >> 24) == 127; }

/// Directed broadcast addresses of all active IPv4 interfaces, plus the limited
/// broadcast address. Windows sends 255.255.255.255 through one adapter only,
/// so machines with VPN or virtual adapters need the per-interface addresses.
std::vector<std::uint32_t> broadcastAddresses();

/// One IPv4 network this machine is attached to.
struct LocalSubnet {
    std::uint32_t address = 0; // our own address in it
    unsigned prefix = 0;       // number of network bits, 24 for 255.255.255.0
    bool tunnel = false;       // a VPN adapter (WireGuard, Tailscale): its network is somewhere else

    friend bool operator==(const LocalSubnet&, const LocalSubnet&) = default;
};

/// The networks of all active adapters, without loopback.
std::vector<LocalSubnet> localSubnets();

/// Whether an address lies in one of these networks on an adapter of this PC that is no tunnel:
/// a console at home, not one reached through a VPN or the internet. Loopback counts as home.
bool isAtHome(std::uint32_t address, const std::vector<LocalSubnet>& subnets);

struct Datagram {
    Bytes data;
    Endpoint from;
};

class UdpSocket {
public:
    struct Options {
        Endpoint bind{};
        bool broadcast = false;
    };

    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    static Result<UdpSocket> open(const Options& options);

    [[nodiscard]] bool isOpen() const;
    void close();

    Result<void> sendTo(std::span<const std::uint8_t> data, const Endpoint& to);

    /// Waits up to `timeout` for one datagram. An empty optional means timeout.
    Result<std::optional<Datagram>> receive(Milliseconds timeout, std::size_t maxSize = 2048);

    [[nodiscard]] Endpoint localEndpoint() const;

private:
    std::intptr_t handle_ = -1;
};

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    static Result<TcpSocket> connect(const Endpoint& to, Milliseconds timeout);

    [[nodiscard]] bool isOpen() const;
    void close();

    Result<void> sendAll(std::span<const std::uint8_t> data, Milliseconds timeout);

    /// Reads at most buffer.size() bytes. Returns 0 when the peer closed the
    /// connection, ErrorCode::Timeout when nothing arrived in time.
    Result<std::size_t> receiveSome(std::span<std::uint8_t> buffer, Milliseconds timeout);

private:
    friend class TcpListener;
    std::intptr_t handle_ = -1;
};

/// Only used by tests and tools that play the console side.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    static Result<TcpListener> listen(const Endpoint& bind);

    void close();
    [[nodiscard]] Endpoint localEndpoint() const;

    /// An empty optional means timeout.
    Result<std::optional<TcpSocket>> accept(Milliseconds timeout);

private:
    std::intptr_t handle_ = -1;
};

} // namespace veloplay::net
