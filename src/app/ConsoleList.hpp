#pragma once

#include <string>
#include <vector>

#include "app/Config.hpp"
#include "protocol/Discovery.hpp"

namespace veloplay {

enum class ConsoleStatus {
    Offline, // no answer: powered off, unplugged or on another network
    Standby, // rest mode, can be woken
    Ready,
};

/// One row on the dashboard: what is stored about a console combined with what
/// discovery currently sees.
struct ConsoleEntry {
    std::string hostId;
    std::string name;
    std::string address; // empty if unknown
    ConsoleStatus status = ConsoleStatus::Offline;
    bool registered = false;       // paired with this PC
    bool credentialsValid = false; // stored keys are readable
    bool waking = false;           // wake-up sent, waiting for it to boot
    std::string runningApp;

    friend bool operator==(const ConsoleEntry&, const ConsoleEntry&) = default;
};

/// Paired consoles come first, in the order they are stored. Consoles that
/// answer discovery but are not paired yet follow.
std::vector<ConsoleEntry> buildConsoleList(const std::vector<RegisteredConsole>& registered,
                                           const std::vector<protocol::DiscoveredHost>& discovered);

} // namespace veloplay
