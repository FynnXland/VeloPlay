#include "protocol/ConnectionProbe.hpp"

#include <algorithm>

#include "net/Socket.hpp"

namespace veloplay::protocol {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

float toMilliseconds(Clock::duration duration) {
    return std::chrono::duration<float, std::milli>(duration).count();
}

} // namespace

ConnectionProbe::ConnectionProbe() : ConnectionProbe(Config{}) {}
ConnectionProbe::ConnectionProbe(Config config) : config_(config) {}
ConnectionProbe::~ConnectionProbe() { stop(); }

Result<void> ConnectionProbe::start(std::uint32_t address) {
    stop();
    {
        std::lock_guard lock(mutex_);
        stats_.clear();
    }
    address_ = address;
    thread_ = std::jthread([this, address](std::stop_token stop) { run(stop, address); });
    return {};
}

void ConnectionProbe::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
}

LatencySnapshot ConnectionProbe::snapshot() const {
    std::lock_guard lock(mutex_);
    return stats_.snapshot();
}

void ConnectionProbe::run(std::stop_token stop, std::uint32_t address) {
    // Its own socket: answers to the regular network search must not be
    // mistaken for answers to a probe.
    auto socket = net::UdpSocket::open({{net::kAnyAddress, 0}, false});
    if (!socket) return;

    const std::string packet = buildSearchRequest();
    const net::Endpoint console{address, config_.consolePort};
    auto next = Clock::now();

    while (!stop.stop_requested()) {
        // Throw away answers that arrived after their probe had timed out.
        for (;;) {
            auto stale = socket->receive(milliseconds(0));
            if (!stale || !stale->has_value()) break;
        }

        const auto sentAt = Clock::now();
        std::optional<float> elapsed;
        if (socket->sendTo(asBytes(packet), console)) {
            const auto deadline = sentAt + config_.timeout;
            while (!elapsed && !stop.stop_requested()) {
                const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - Clock::now());
                if (remaining.count() <= 0) break;
                auto datagram = socket->receive(std::min(remaining, milliseconds(50)));
                if (!datagram) break;
                if (!datagram->has_value()) continue;
                const auto& received = **datagram;
                if (received.from.address == address && parseDiscoveryResponse(asText(received.data))) {
                    elapsed = toMilliseconds(Clock::now() - sentAt);
                }
            }
        }
        if (stop.stop_requested()) break;
        {
            std::lock_guard lock(mutex_);
            stats_.add(elapsed);
        }

        next += config_.interval;
        const auto now = Clock::now();
        if (next < now) next = now; // never try to catch up with a burst
        while (!stop.stop_requested() && Clock::now() < next) {
            std::this_thread::sleep_for(std::min<Clock::duration>(next - Clock::now(), milliseconds(20)));
        }
    }
}

} // namespace veloplay::protocol
