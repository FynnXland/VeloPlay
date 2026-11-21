#include "protocol/RegistrationClient.hpp"

#include <algorithm>
#include <charconv>
#include <thread>

#include <spdlog/spdlog.h>

#include "net/Socket.hpp"
#include "protocol/HttpMessage.hpp"

namespace veloplay::protocol {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr std::size_t kMaxResponseSize = 4096;
// The console needs a moment between answering the search and accepting the
// TCP connection.
constexpr milliseconds kPauseAfterSearch{100};

Error canceled() { return Error{ErrorCode::Canceled, "pairing was cancelled"}; }

/// Announces the pairing attempt and waits for the console to acknowledge it.
Result<void> searchConsole(const RegistrationParams& params, std::stop_token stop) {
    auto socket = net::UdpSocket::open({{net::kAnyAddress, 0}, false});
    if (!socket) return socket.error();

    const net::Endpoint console{params.address, params.port};
    if (const auto sent = socket->sendTo(asBytes(kRegistSearchRequest), console); !sent) return sent.error();

    const auto deadline = Clock::now() + params.searchTimeout;
    while (Clock::now() < deadline) {
        if (stop.stop_requested()) return canceled();
        auto datagram = socket->receive(milliseconds(100));
        if (!datagram) return datagram.error();
        if (!datagram->has_value()) continue;
        const auto& received = **datagram;
        if (received.from.address == params.address && asText(received.data).starts_with(kRegistSearchResponse)) {
            return {};
        }
    }
    return Error{ErrorCode::Timeout,
                 "the console did not answer the pairing request; its Link Device screen must stay open"};
}

} // namespace

Result<RegisteredHost> registerWithConsole(const RegistrationParams& params, std::stop_token stop) {
    auto request = buildRegistRequest(params.account, params.pin);
    if (!request) return request.error();

    if (const auto found = searchConsole(params, stop); !found) return found.error();
    std::this_thread::sleep_for(kPauseAfterSearch);
    if (stop.stop_requested()) return canceled();

    auto socket = net::TcpSocket::connect({params.address, params.port}, params.responseTimeout);
    if (!socket) return socket.error();
    spdlog::info("Pairing: connected to {}, sending request", net::formatIpv4(params.address));

    if (const auto sent = socket->sendAll(asBytes(request->httpHeader), params.responseTimeout); !sent) return sent.error();
    if (const auto sent = socket->sendAll(request->payload, params.responseTimeout); !sent) return sent.error();

    // Read the HTTP header, then exactly Content-Length body bytes.
    Bytes buffer;
    std::optional<std::size_t> headerEnd;
    std::size_t expectedTotal = 0;
    const auto deadline = Clock::now() + params.responseTimeout;

    for (;;) {
        if (stop.stop_requested()) return canceled();
        if (headerEnd && buffer.size() >= expectedTotal) break;

        const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) return Error{ErrorCode::Timeout, "the console did not finish its pairing answer"};

        std::uint8_t chunk[1024];
        const auto received = socket->receiveSome(chunk, std::min(remaining, milliseconds(200)));
        if (!received) {
            if (received.error().code == ErrorCode::Timeout) continue;
            return received.error();
        }
        if (*received == 0) {
            if (headerEnd && buffer.size() >= expectedTotal) break;
            return Error{ErrorCode::Network, "the console closed the connection during pairing"};
        }
        buffer.insert(buffer.end(), chunk, chunk + *received);
        if (buffer.size() > kMaxResponseSize) return Error{ErrorCode::InvalidData, "pairing answer is too large"};

        if (!headerEnd) {
            headerEnd = findHeaderEnd(buffer);
            if (!headerEnd) continue;

            const auto http = parseHttpResponse(asText(buffer).substr(0, *headerEnd));
            if (!http) return Error{ErrorCode::InvalidData, "pairing answer is not a valid HTTP response"};

            if (http->status != 200) {
                std::string message = "the console rejected the pairing (HTTP " + std::to_string(http->status) + ")";
                if (const auto reason = http->headers.get("RP-Application-Reason")) {
                    std::uint32_t code = 0;
                    std::from_chars(reason->data(), reason->data() + reason->size(), code, 16);
                    message += ": " + describeApplicationReason(code);
                }
                return Error{ErrorCode::Rejected, std::move(message)};
            }

            std::size_t contentLength = 0;
            if (const auto length = http->headers.get("Content-Length")) {
                std::from_chars(length->data(), length->data() + length->size(), contentLength);
            }
            // Compared this way round so a huge Content-Length cannot overflow the sum.
            if (contentLength == 0 || *headerEnd > kMaxResponseSize || contentLength > kMaxResponseSize - *headerEnd) {
                return Error{ErrorCode::InvalidData, "pairing answer has an invalid Content-Length"};
            }
            expectedTotal = *headerEnd + contentLength;
        }
    }

    const std::span<const std::uint8_t> body(buffer.data() + *headerEnd, expectedTotal - *headerEnd);
    const auto decrypted = request->cipher.decrypt(body);
    if (!decrypted) return decrypted.error();

    auto host = parseRegistResponse(asText(*decrypted));
    if (host) spdlog::info("Pairing: registered with \"{}\" ({})", host->nickname, host->hostId());
    return host;
}

} // namespace veloplay::protocol
