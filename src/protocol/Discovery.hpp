#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

#include "net/Socket.hpp"

// LAN discovery and wake-up. The console listens on UDP 9302 and answers a
// "SRCH" datagram with an HTTP-like status: 200 when it is running, 620 when it
// is in rest mode. See docs/protocol_notes.md.

namespace veloplay::protocol {

inline constexpr std::uint16_t kDiscoveryPortPs5 = 9302;
inline constexpr std::string_view kDiscoveryProtocolVersionPs5 = "00030010";

enum class HostState {
    Unknown,
    Ready,   // HTTP-like status 200
    Standby, // status 620, rest mode
};

struct DiscoveryResponse {
    HostState state = HostState::Unknown;
    int statusCode = 0;
    std::string hostId;   // MAC address, 12 hex digits
    std::string hostName; // name set on the console
    std::string hostType; // "PS5" or "PS4"
    std::string systemVersion;
    std::string protocolVersion;
    std::string runningAppName;
    std::string runningAppTitleId;
    std::uint16_t requestPort = 0;

    [[nodiscard]] bool isPs5() const { return protocolVersion == kDiscoveryProtocolVersionPs5; }

    friend bool operator==(const DiscoveryResponse&, const DiscoveryResponse&) = default;
};

/// Datagram payload for a search. The trailing NUL is part of the message.
std::string buildSearchRequest(std::string_view protocolVersion = kDiscoveryProtocolVersionPs5);

/// Datagram payload that wakes a console from rest mode. `credential` comes
/// from wakeCredential(); the console ignores requests with a wrong one.
std::string buildWakeupRequest(std::uint64_t credential,
                               std::string_view protocolVersion = kDiscoveryProtocolVersionPs5);

std::optional<DiscoveryResponse> parseDiscoveryResponse(std::string_view datagram);

/// The registration key is a short hex string (for example "1b3d4f5a"). The
/// wake-up packet carries the same number in decimal.
std::optional<std::uint64_t> wakeCredential(std::string_view registKey);

struct DiscoveredHost {
    DiscoveryResponse info;
    std::uint32_t address = 0; // host byte order
    std::chrono::steady_clock::time_point lastSeen{};
};

/// Collects discovery answers and forgets hosts that stop answering.
/// Not thread-safe; DiscoveryService guards it.
class HostTracker {
public:
    using Clock = std::chrono::steady_clock;

    explicit HostTracker(Clock::duration timeout = std::chrono::seconds(7)) : timeout_(timeout) {}

    void update(const DiscoveryResponse& response, std::uint32_t address, Clock::time_point now);

    /// Hosts seen within the timeout, ordered by name, then address.
    [[nodiscard]] std::vector<DiscoveredHost> snapshot(Clock::time_point now) const;

    void prune(Clock::time_point now);

private:
    Clock::duration timeout_;
    std::map<std::string, DiscoveredHost> hosts_;
};

/// Every other address in the given networks, for asking each one directly.
/// Networks with more than 254 addresses are left out: too many to ask.
std::vector<std::uint32_t> sweepAddresses(std::span<const net::LocalSubnet> subnets);

} // namespace veloplay::protocol
