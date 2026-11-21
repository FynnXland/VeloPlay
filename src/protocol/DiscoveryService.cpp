#include "protocol/DiscoveryService.hpp"

#include <string>

#include <spdlog/spdlog.h>

namespace veloplay::protocol {

namespace {

Result<net::UdpSocket> openDiscoverySocket(const DiscoveryService::Config& config) {
    if (config.localPortFirst != 0) {
        for (std::uint32_t port = config.localPortFirst; port <= config.localPortLast; ++port) {
            auto socket = net::UdpSocket::open({{net::kAnyAddress, static_cast<std::uint16_t>(port)}, config.broadcast});
            if (socket) return socket;
        }
    }
    return net::UdpSocket::open({{net::kAnyAddress, 0}, config.broadcast});
}

} // namespace

DiscoveryService::DiscoveryService() : DiscoveryService(Config{}) {}

DiscoveryService::DiscoveryService(Config config) : config_(config), tracker_(config.hostTimeout) {}

DiscoveryService::~DiscoveryService() { stop(); }

Result<void> DiscoveryService::start() {
    if (running()) return {};
    auto socket = openDiscoverySocket(config_);
    if (!socket) return socket.error();
    {
        std::lock_guard lock(mutex_);
        socket_ = std::move(*socket);
    }
    spdlog::info("Discovery listening on UDP port {}", socket_.localEndpoint().port);
    std::string networks;
    for (const net::LocalSubnet& subnet : net::localSubnets()) {
        networks += (networks.empty() ? "" : ", ") + net::formatIpv4(subnet.address) + "/" + std::to_string(subnet.prefix);
    }
    spdlog::info("Local networks: {}", networks.empty() ? "none" : networks);
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    return {};
}

void DiscoveryService::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
    std::lock_guard lock(mutex_);
    socket_.close();
}

void DiscoveryService::setDirectTargets(std::vector<std::uint32_t> addresses) {
    std::lock_guard lock(mutex_);
    directTargets_ = std::move(addresses);
}

std::vector<DiscoveredHost> DiscoveryService::hosts() const {
    std::lock_guard lock(mutex_);
    return tracker_.snapshot(HostTracker::Clock::now());
}

Result<void> DiscoveryService::wake(std::uint32_t address, std::uint64_t credential) {
    const std::string packet = buildWakeupRequest(credential);
    const net::Endpoint target{address, config_.consolePort};

    if (running()) return socket_.sendTo(asBytes(packet), target);

    auto socket = net::UdpSocket::open({{net::kAnyAddress, 0}, true});
    if (!socket) return socket.error();
    return socket->sendTo(asBytes(packet), target);
}

void DiscoveryService::sendSweep() {
    const std::vector<net::LocalSubnet> subnets = config_.sweepSubnets.empty() ? net::localSubnets()
                                                                               : config_.sweepSubnets;
    const std::vector<std::uint32_t> targets = sweepAddresses(subnets);
    if (targets.empty()) return;
    spdlog::info("Discovery: nothing answered the broadcast, asking {} addresses directly", targets.size());
    const std::string packet = buildSearchRequest();
    for (std::uint32_t address : targets) {
        (void)socket_.sendTo(asBytes(packet), {address, config_.consolePort});
    }
}

void DiscoveryService::sendSearch() {
    std::vector<std::uint32_t> targets;
    if (config_.broadcast) targets = net::broadcastAddresses();
    {
        std::lock_guard lock(mutex_);
        targets.insert(targets.end(), directTargets_.begin(), directTargets_.end());
        // Consoles that are known are also asked directly, so one that was
        // found by a sweep stays on the list where the broadcast fails.
        for (const DiscoveredHost& host : tracker_.snapshot(HostTracker::Clock::now())) targets.push_back(host.address);
    }

    const std::string packet = buildSearchRequest();
    for (std::uint32_t address : targets) {
        // Failures are expected here: adapters without a route, hosts that are
        // switched off. The next round tries again.
        const auto sent = socket_.sendTo(asBytes(packet), {address, config_.consolePort});
        if (!sent) spdlog::debug("Discovery probe to {} failed: {}", net::formatIpv4(address), sent.error().message);
    }
}

void DiscoveryService::run(std::stop_token stop) {
    using Clock = HostTracker::Clock;
    auto nextSearch = Clock::now();
    auto nextSweep = Clock::now() + config_.sweepAfter;
    // Sweeping real networks only makes sense where the broadcast is used too.
    const bool sweeping = config_.sweep && (config_.broadcast || !config_.sweepSubnets.empty());
    std::size_t answers = 0;
    std::size_t unreadable = 0;

    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        if (now >= nextSearch) {
            sendSearch();
            nextSearch = now + config_.searchInterval;
            std::lock_guard lock(mutex_);
            tracker_.prune(now);
        }
        if (sweeping && now >= nextSweep) {
            nextSweep = now + config_.sweepInterval;
            if (hosts().empty()) sendSweep();
        }

        auto datagram = socket_.receive(std::chrono::milliseconds(100));
        if (!datagram) {
            spdlog::warn("Discovery receive failed: {}", datagram.error().message);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (!datagram->has_value()) continue;

        const auto& received = **datagram;
        const auto response = parseDiscoveryResponse(asText(received.data));
        if (!response) {
            // Worth one line: it shows that packets arrive but are not understood.
            if (unreadable++ == 0) {
                spdlog::warn("Discovery: {} sent {} bytes that are not a console's answer",
                             net::formatIpv4(received.from.address), received.data.size());
            }
            continue;
        }
        if (answers++ == 0) {
            spdlog::info("Discovery: first answer, from {} ({})", net::formatIpv4(received.from.address),
                         response->hostName);
        }

        std::lock_guard lock(mutex_);
        tracker_.update(*response, received.from.address, Clock::now());
    }
}

} // namespace veloplay::protocol
