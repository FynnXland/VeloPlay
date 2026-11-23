#include "net/Socket.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET // missing from some SDK flavours
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace veloplay::net {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using SockLen = int;
const NativeSocket kInvalidSocket = INVALID_SOCKET;

void ensureNetworkInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

int lastError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
void closeNative(NativeSocket s) { closesocket(s); }

bool setNonBlocking(NativeSocket s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

std::string describeError(int error) {
    return "winsock error " + std::to_string(error);
}
#else
using NativeSocket = int;
using SockLen = socklen_t;
const NativeSocket kInvalidSocket = -1;

void ensureNetworkInitialized() {}
int lastError() { return errno; }
bool wouldBlock(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS; }
void closeNative(NativeSocket s) { ::close(s); }

bool setNonBlocking(NativeSocket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string describeError(int error) {
    return std::strerror(error);
}
#endif

NativeSocket native(std::intptr_t handle) { return static_cast<NativeSocket>(handle); }
std::intptr_t toHandle(NativeSocket s) { return static_cast<std::intptr_t>(s); }

sockaddr_in toSockaddr(const Endpoint& endpoint) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    addr.sin_addr.s_addr = htonl(endpoint.address);
    return addr;
}

Endpoint fromSockaddr(const sockaddr_in& addr) {
    return Endpoint{ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port)};
}

Error networkError(std::string_view what) {
    return Error{ErrorCode::Network, std::string(what) + ": " + describeError(lastError())};
}

enum class WaitFor { Read, Write };

/// Returns 1 when ready, 0 on timeout, -1 on error.
int waitSocket(NativeSocket s, WaitFor what, Milliseconds timeout) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    fd_set errors;
    FD_ZERO(&errors);
    FD_SET(s, &errors);

    const auto clamped = std::max<std::int64_t>(0, timeout.count());
    timeval tv;
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(clamped / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((clamped % 1000) * 1000);

    const int nfds = static_cast<int>(s) + 1; // ignored by Winsock
    const int r = ::select(nfds, what == WaitFor::Read ? &set : nullptr,
                           what == WaitFor::Write ? &set : nullptr, &errors, &tv);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return 1;
}

Endpoint localEndpointOf(std::intptr_t handle) {
    if (handle < 0) return {};
    sockaddr_in addr{};
    SockLen len = sizeof(addr);
    if (getsockname(native(handle), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return {};
    return fromSockaddr(addr);
}

} // namespace

std::optional<std::uint32_t> parseIpv4(std::string_view text) {
    std::uint32_t result = 0;
    int parts = 0;
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        unsigned value = 0;
        const auto [next, ec] = std::from_chars(p, end, value);
        if (ec != std::errc{} || next == p || next - p > 3 || value > 255) return std::nullopt;
        result = (result << 8) | value;
        ++parts;
        p = next;
        if (p == end) break;
        if (*p != '.' || parts == 4) return std::nullopt;
        ++p;
        if (p == end) return std::nullopt;
    }
    if (parts != 4) return std::nullopt;
    return result;
}

std::string formatIpv4(std::uint32_t address) {
    return std::to_string((address >> 24) & 0xff) + "." + std::to_string((address >> 16) & 0xff) + "." +
           std::to_string((address >> 8) & 0xff) + "." + std::to_string(address & 0xff);
}

std::string toString(const Endpoint& endpoint) {
    return formatIpv4(endpoint.address) + ":" + std::to_string(endpoint.port);
}

std::vector<LocalSubnet> localSubnets() {
    ensureNetworkInitialized();
    std::vector<LocalSubnet> result;

#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                // Wintun, the adapter of WireGuard and Tailscale, says IF_TYPE_PROP_VIRTUAL (53; wintun.inf, *IfType).
                const bool tunnel = adapter->IfType == IF_TYPE_PROP_VIRTUAL || adapter->IfType == IF_TYPE_TUNNEL;
                result.push_back(
                    {ntohl(addr->sin_addr.s_addr), static_cast<unsigned>(unicast->OnLinkPrefixLength), tunnel});
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || !it->ifa_netmask) continue;
            if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
            const std::uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
            std::uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr);
            unsigned prefix = 0;
            for (; mask & 0x80000000u; mask <<= 1) ++prefix;
            const std::string name = it->ifa_name ? it->ifa_name : "";
            const bool tunnel = (it->ifa_flags & IFF_POINTOPOINT) != 0 || name.rfind("wg", 0) == 0 ||
                                name.rfind("tun", 0) == 0 || name.rfind("tailscale", 0) == 0;
            result.push_back({ip, prefix, tunnel});
        }
        freeifaddrs(list);
    }
#endif
    return result;
}

bool isAtHome(std::uint32_t address, const std::vector<LocalSubnet>& subnets) {
    if ((address >> 24) == 127) return true;
    for (const LocalSubnet& subnet : subnets) {
        if (subnet.tunnel || subnet.prefix == 0 || subnet.prefix > 32) continue;
        const std::uint32_t mask = subnet.prefix == 32 ? 0xffffffffu : ~(0xffffffffu >> subnet.prefix);
        if ((address & mask) == (subnet.address & mask)) return true;
    }
    return false;
}

std::vector<std::uint32_t> broadcastAddresses() {
    ensureNetworkInitialized();
    std::vector<std::uint32_t> result;

#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                const std::uint32_t ip = ntohl(addr->sin_addr.s_addr);
                const unsigned prefix = unicast->OnLinkPrefixLength;
                if (prefix == 0 || prefix >= 31) continue;
                const std::uint32_t mask = 0xffffffffu << (32 - prefix);
                result.push_back(ip | ~mask);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            if (!(it->ifa_flags & IFF_UP) || !(it->ifa_flags & IFF_BROADCAST) || (it->ifa_flags & IFF_LOOPBACK)) continue;
            if (!it->ifa_broadaddr) continue;
            const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_broadaddr);
            result.push_back(ntohl(addr->sin_addr.s_addr));
        }
        freeifaddrs(list);
    }
#endif

    result.push_back(kBroadcastAddress);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// ---- UdpSocket ---------------------------------------------------------

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

Result<UdpSocket> UdpSocket::open(const Options& options) {
    ensureNetworkInitialized();
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) return networkError("create UDP socket");

    UdpSocket socket;
    socket.handle_ = toHandle(s);

    if (options.broadcast) {
        const int enable = 1;
        if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable), sizeof(enable)) != 0) {
            return networkError("enable broadcast");
        }
    }

#ifdef _WIN32
    // Without this, an ICMP "port unreachable" for an earlier sendto() makes the
    // next recvfrom() fail with WSAECONNRESET. Discovery probes offline hosts all
    // the time, so that error would be noise.
    BOOL reportReset = FALSE;
    DWORD ignored = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &ignored, nullptr, nullptr);
#endif

    const sockaddr_in addr = toSockaddr(options.bind);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return networkError("bind UDP socket to " + toString(options.bind));
    }
    return socket;
}

bool UdpSocket::isOpen() const { return handle_ >= 0; }

void UdpSocket::close() {
    if (handle_ >= 0) {
        closeNative(native(handle_));
        handle_ = -1;
    }
}

Result<void> UdpSocket::sendTo(std::span<const std::uint8_t> data, const Endpoint& to) {
    if (!isOpen()) return Error{ErrorCode::Network, "UDP socket is closed"};
    const sockaddr_in addr = toSockaddr(to);
    const auto sent = ::sendto(native(handle_), reinterpret_cast<const char*>(data.data()),
                               static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&addr),
                               sizeof(addr));
    if (sent < 0) return networkError("send datagram to " + toString(to));
    return {};
}

Result<std::optional<Datagram>> UdpSocket::receive(Milliseconds timeout, std::size_t maxSize) {
    if (!isOpen()) return Error{ErrorCode::Network, "UDP socket is closed"};
    const int ready = waitSocket(native(handle_), WaitFor::Read, timeout);
    if (ready < 0) return networkError("wait for datagram");
    if (ready == 0) return std::optional<Datagram>{};

    Datagram datagram;
    datagram.data.resize(maxSize);
    sockaddr_in from{};
    SockLen fromLen = sizeof(from);
    const auto received = ::recvfrom(native(handle_), reinterpret_cast<char*>(datagram.data.data()),
                                     static_cast<int>(datagram.data.size()), 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (received < 0) {
#ifdef _WIN32
        // Oversized datagram: not ours, treat like nothing arrived.
        if (lastError() == WSAEMSGSIZE) return std::optional<Datagram>{};
#endif
        return networkError("receive datagram");
    }
    datagram.data.resize(static_cast<std::size_t>(received));
    datagram.from = fromSockaddr(from);
    return std::optional<Datagram>{std::move(datagram)};
}

Endpoint UdpSocket::localEndpoint() const { return localEndpointOf(handle_); }

// ---- TcpSocket ---------------------------------------------------------

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

Result<TcpSocket> TcpSocket::connect(const Endpoint& to, Milliseconds timeout) {
    ensureNetworkInitialized();
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) return networkError("create TCP socket");

    TcpSocket socket;
    socket.handle_ = toHandle(s);
    if (!setNonBlocking(s)) return networkError("set TCP socket non-blocking");

    const sockaddr_in addr = toSockaddr(to);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (!wouldBlock(lastError())) return networkError("connect to " + toString(to));
        const int ready = waitSocket(s, WaitFor::Write, timeout);
        if (ready < 0) return networkError("connect to " + toString(to));
        if (ready == 0) return Error{ErrorCode::Timeout, "connect to " + toString(to) + " timed out"};

        int soError = 0;
        SockLen len = sizeof(soError);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) {
            return Error{ErrorCode::Network, "connect to " + toString(to) + ": " + describeError(soError)};
        }
    }

    const int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return socket;
}

bool TcpSocket::isOpen() const { return handle_ >= 0; }

void TcpSocket::close() {
    if (handle_ >= 0) {
        closeNative(native(handle_));
        handle_ = -1;
    }
}

Result<void> TcpSocket::sendAll(std::span<const std::uint8_t> data, Milliseconds timeout) {
    if (!isOpen()) return Error{ErrorCode::Network, "TCP socket is closed"};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sentTotal = 0;
#ifdef _WIN32
    const int flags = 0;
#else
    const int flags = MSG_NOSIGNAL;
#endif
    while (sentTotal < data.size()) {
        const auto sent = ::send(native(handle_), reinterpret_cast<const char*>(data.data() + sentTotal),
                                 static_cast<int>(data.size() - sentTotal), flags);
        if (sent > 0) {
            sentTotal += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && !wouldBlock(lastError())) return networkError("send");

        const auto remaining = std::chrono::duration_cast<Milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return Error{ErrorCode::Timeout, "send timed out"};
        if (waitSocket(native(handle_), WaitFor::Write, remaining) < 0) return networkError("send");
    }
    return {};
}

Result<std::size_t> TcpSocket::receiveSome(std::span<std::uint8_t> buffer, Milliseconds timeout) {
    if (!isOpen()) return Error{ErrorCode::Network, "TCP socket is closed"};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining = std::chrono::duration_cast<Milliseconds>(deadline - std::chrono::steady_clock::now());
        const int ready = waitSocket(native(handle_), WaitFor::Read, std::max(remaining, Milliseconds{0}));
        if (ready < 0) return networkError("receive");
        if (ready == 0) return Error{ErrorCode::Timeout, "receive timed out"};

        const auto received = ::recv(native(handle_), reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()), 0);
        if (received >= 0) return static_cast<std::size_t>(received);
        if (!wouldBlock(lastError())) return networkError("receive");
    }
}

// ---- TcpListener -------------------------------------------------------

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

Result<TcpListener> TcpListener::listen(const Endpoint& bind) {
    ensureNetworkInitialized();
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) return networkError("create TCP listener");

    TcpListener listener;
    listener.handle_ = toHandle(s);

    const sockaddr_in addr = toSockaddr(bind);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return networkError("bind TCP listener to " + toString(bind));
    }
    if (::listen(s, 4) != 0) return networkError("listen");
    return listener;
}

void TcpListener::close() {
    if (handle_ >= 0) {
        closeNative(native(handle_));
        handle_ = -1;
    }
}

Endpoint TcpListener::localEndpoint() const { return localEndpointOf(handle_); }

Result<std::optional<TcpSocket>> TcpListener::accept(Milliseconds timeout) {
    if (handle_ < 0) return Error{ErrorCode::Network, "TCP listener is closed"};
    const int ready = waitSocket(native(handle_), WaitFor::Read, timeout);
    if (ready < 0) return networkError("wait for connection");
    if (ready == 0) return std::optional<TcpSocket>{};

    const NativeSocket s = ::accept(native(handle_), nullptr, nullptr);
    if (s == kInvalidSocket) return networkError("accept");
    TcpSocket socket;
    socket.handle_ = toHandle(s);
    setNonBlocking(s);
    return std::optional<TcpSocket>{std::move(socket)};
}

} // namespace veloplay::net
