#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "net/Socket.hpp"
#include "protocol/Discovery.hpp"
#include "util/Result.hpp"

namespace veloplay::protocol {

/// Searches the local network in the background and keeps a list of consoles
/// that answered recently.
class DiscoveryService {
public:
    struct Config {
        std::uint16_t consolePort = kDiscoveryPortPs5;
        // The official client sends from 9303..9319. Firewalls set up for it
        // expect that range, so try it first and fall back to any free port.
        std::uint16_t localPortFirst = 9303;
        std::uint16_t localPortLast = 9319;
        std::chrono::milliseconds searchInterval{2000};
        std::chrono::milliseconds hostTimeout{7000};
        bool broadcast = true;

        // The broadcast does not always work: some Wi-Fi access points and
        // repeaters do not pass it on, and a firewall may drop the answer,
        // because it comes from another address than the one that was asked.
        // When no console is known, every address of the local networks is
        // therefore asked directly from time to time. An answer to a direct
        // question passes any firewall.
        bool sweep = true;
        std::chrono::milliseconds sweepAfter{4000};     // first sweep after this long without a console
        std::chrono::milliseconds sweepInterval{10000}; // and then this often
        /// Networks to sweep instead of the ones the operating system reports. For tests.
        std::vector<net::LocalSubnet> sweepSubnets;
    };

    DiscoveryService();
    explicit DiscoveryService(Config config);
    ~DiscoveryService();

    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    Result<void> start();
    void stop();
    [[nodiscard]] bool running() const { return thread_.joinable(); }

    /// Addresses probed directly in addition to the broadcast. Use this for
    /// consoles behind a router that drops broadcasts (VLANs, some mesh setups).
    void setDirectTargets(std::vector<std::uint32_t> addresses);

    [[nodiscard]] std::vector<DiscoveredHost> hosts() const;

    /// Sends one wake-up packet. The console needs several seconds to boot;
    /// watch hosts() for its state to turn Ready.
    Result<void> wake(std::uint32_t address, std::uint64_t credential);

private:
    void run(std::stop_token stop);
    void sendSearch();
    void sendSweep();

    Config config_;
    mutable std::mutex mutex_;
    net::UdpSocket socket_;
    HostTracker tracker_;
    std::vector<std::uint32_t> directTargets_;
    std::jthread thread_;
};

} // namespace veloplay::protocol
