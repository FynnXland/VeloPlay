#include "app/ConsoleList.hpp"

#include <algorithm>

#include "net/Socket.hpp"

namespace veloplay {

namespace {

ConsoleStatus toStatus(protocol::HostState state) {
    switch (state) {
        case protocol::HostState::Ready: return ConsoleStatus::Ready;
        case protocol::HostState::Standby: return ConsoleStatus::Standby;
        case protocol::HostState::Unknown: break;
    }
    return ConsoleStatus::Offline;
}

} // namespace

std::vector<ConsoleEntry> buildConsoleList(const std::vector<RegisteredConsole>& registered,
                                           const std::vector<protocol::DiscoveredHost>& discovered) {
    std::vector<ConsoleEntry> list;
    std::vector<bool> used(discovered.size(), false);

    for (const RegisteredConsole& console : registered) {
        ConsoleEntry entry;
        entry.hostId = console.hostId;
        entry.name = console.name;
        entry.address = console.lastAddress;
        entry.registered = true;
        entry.credentialsValid = console.credentialsValid;

        for (std::size_t i = 0; i < discovered.size(); ++i) {
            if (!equalsIgnoreCase(discovered[i].info.hostId, console.hostId)) continue;
            used[i] = true;
            entry.address = net::formatIpv4(discovered[i].address);
            entry.status = toStatus(discovered[i].info.state);
            entry.runningApp = discovered[i].info.runningAppName;
            if (!discovered[i].info.hostName.empty()) entry.name = discovered[i].info.hostName;
            break;
        }
        list.push_back(std::move(entry));
    }

    for (std::size_t i = 0; i < discovered.size(); ++i) {
        if (used[i]) continue;
        const protocol::DiscoveredHost& host = discovered[i];
        ConsoleEntry entry;
        entry.hostId = host.info.hostId;
        entry.name = host.info.hostName.empty() ? host.info.hostType : host.info.hostName;
        if (entry.name.empty()) entry.name = "PS5";
        entry.address = net::formatIpv4(host.address);
        entry.status = toStatus(host.info.state);
        entry.runningApp = host.info.runningAppName;
        list.push_back(std::move(entry));
    }
    return list;
}

} // namespace veloplay
