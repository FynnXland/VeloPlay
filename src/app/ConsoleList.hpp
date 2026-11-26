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
    std::string runningAppId; // the game's title id, like "PPSA01325"; tells games apart
    /// Not in this network, but Sony's servers know it with Remote Play on: "Connect" goes that way.
    bool viaInternet = false;
    /// Not paired here, but Sony's servers list it for the signed-in account with Remote Play on: it can be
    /// paired through them without a PIN. Sony's id of the console in hex; empty otherwise. A row for a console
    /// only Sony knows has the hostId "sony:" followed by the same.
    std::string sonyId;

    friend bool operator==(const ConsoleEntry&, const ConsoleEntry&) = default;
};

/// Paired consoles come first, in the order they are stored. Consoles that
/// answer discovery but are not paired yet follow.
std::vector<ConsoleEntry> buildConsoleList(const std::vector<RegisteredConsole>& registered,
                                           const std::vector<protocol::DiscoveredHost>& discovered);

} // namespace veloplay
