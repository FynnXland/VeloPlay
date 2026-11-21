#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>

#include "protocol/Discovery.hpp"
#include "protocol/LatencyStats.hpp"
#include "util/Result.hpp"

namespace veloplay::protocol {

/// Measures the link to one console by sending it discovery packets at a steady
/// pace and timing the answers. Works for consoles in rest mode too, and needs
/// no pairing.
class ConnectionProbe {
public:
    struct Config {
        std::uint16_t consolePort = kDiscoveryPortPs5;
        std::chrono::milliseconds interval{400};
        std::chrono::milliseconds timeout{300}; // later answers count as lost
    };

    ConnectionProbe();
    explicit ConnectionProbe(Config config);
    ~ConnectionProbe();

    ConnectionProbe(const ConnectionProbe&) = delete;
    ConnectionProbe& operator=(const ConnectionProbe&) = delete;

    /// Starts measuring `address` (host byte order). A running test is replaced.
    Result<void> start(std::uint32_t address);
    void stop();

    [[nodiscard]] bool running() const { return thread_.joinable(); }
    [[nodiscard]] std::uint32_t address() const { return address_; }
    [[nodiscard]] LatencySnapshot snapshot() const;

private:
    void run(std::stop_token stop, std::uint32_t address);

    Config config_;
    mutable std::mutex mutex_;
    LatencyStats stats_;
    std::uint32_t address_ = 0;
    std::jthread thread_;
};

} // namespace veloplay::protocol
