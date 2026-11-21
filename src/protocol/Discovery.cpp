#include "protocol/Discovery.hpp"

#include <algorithm>
#include <charconv>

#include "net/Socket.hpp"
#include "protocol/HttpMessage.hpp"

namespace veloplay::protocol {

std::string buildSearchRequest(std::string_view protocolVersion) {
    std::string request = "SRCH * HTTP/1.1\n";
    request += "device-discovery-protocol-version:";
    request += protocolVersion;
    request += '\n';
    request.push_back('\0');
    return request;
}

std::string buildWakeupRequest(std::uint64_t credential, std::string_view protocolVersion) {
    std::string request = "WAKEUP * HTTP/1.1\n"
                          "client-type:vr\n"
                          "auth-type:R\n"
                          "model:w\n"
                          "app-type:r\n";
    request += "user-credential:" + std::to_string(credential) + "\n";
    request += "device-discovery-protocol-version:";
    request += protocolVersion;
    request += '\n';
    request.push_back('\0');
    return request;
}

std::optional<DiscoveryResponse> parseDiscoveryResponse(std::string_view datagram) {
    const auto http = parseHttpResponse(datagram);
    if (!http) return std::nullopt;

    DiscoveryResponse response;
    response.statusCode = http->status;
    switch (http->status) {
        case 200: response.state = HostState::Ready; break;
        case 620: response.state = HostState::Standby; break;
        default: response.state = HostState::Unknown; break;
    }

    const auto text = [&](std::string_view name) {
        const auto value = http->headers.get(name);
        return value ? std::string(*value) : std::string{};
    };
    response.hostId = text("host-id");
    response.hostName = text("host-name");
    response.hostType = text("host-type");
    response.systemVersion = text("system-version");
    response.protocolVersion = text("device-discovery-protocol-version");
    response.runningAppName = text("running-app-name");
    response.runningAppTitleId = text("running-app-titleid");

    if (const auto port = http->headers.get("host-request-port")) {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(port->data(), port->data() + port->size(), value);
        if (ec == std::errc{} && value <= 0xffff) response.requestPort = static_cast<std::uint16_t>(value);
    }
    return response;
}

std::optional<std::uint64_t> wakeCredential(std::string_view registKey) {
    if (registKey.empty() || registKey.size() > 16) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(registKey.data(), registKey.data() + registKey.size(), value, 16);
    if (ec != std::errc{} || end != registKey.data() + registKey.size()) return std::nullopt;
    return value;
}

void HostTracker::update(const DiscoveryResponse& response, std::uint32_t address, Clock::time_point now) {
    // The MAC address survives DHCP changes; fall back to the IP if a console
    // ever answers without one.
    const std::string key = response.hostId.empty() ? "ip:" + net::formatIpv4(address) : response.hostId;
    hosts_[key] = DiscoveredHost{response, address, now};
}

std::vector<DiscoveredHost> HostTracker::snapshot(Clock::time_point now) const {
    std::vector<DiscoveredHost> result;
    for (const auto& [key, host] : hosts_) {
        if (now - host.lastSeen <= timeout_) result.push_back(host);
    }
    std::sort(result.begin(), result.end(), [](const DiscoveredHost& a, const DiscoveredHost& b) {
        if (a.info.hostName != b.info.hostName) return a.info.hostName < b.info.hostName;
        return a.address < b.address;
    });
    return result;
}

void HostTracker::prune(Clock::time_point now) {
    std::erase_if(hosts_, [&](const auto& entry) { return now - entry.second.lastSeen > timeout_; });
}

std::vector<std::uint32_t> sweepAddresses(std::span<const net::LocalSubnet> subnets) {
    std::vector<std::uint32_t> result;
    for (const net::LocalSubnet& subnet : subnets) {
        if (subnet.prefix < 24 || subnet.prefix > 30) continue;
        const std::uint32_t mask = 0xffffffffu << (32 - subnet.prefix);
        const std::uint32_t network = subnet.address & mask;
        const std::uint32_t broadcast = network | ~mask;
        for (std::uint32_t address = network + 1; address < broadcast; ++address) {
            if (address != subnet.address) result.push_back(address);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace veloplay::protocol
