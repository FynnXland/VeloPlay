#pragma once

// A stand-in for a PS5 on the loopback interface. It plays the console side of
// discovery, wake-up and registration so the client code can run end to end.
// It shares the cipher with the client, so it proves that both halves fit
// together. That the bytes match a real console is the job of
// reference_vectors.hpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "net/Socket.hpp"
#include "protocol/Discovery.hpp"
#include "protocol/HttpMessage.hpp"
#include "protocol/Registration.hpp"
#include "util/Bytes.hpp"

namespace veloplay::test {

class MockConsole {
public:
    struct Config {
        std::string hostId = "001122AABBCC";
        std::string hostName = "PS5-042";
        bool standby = false;
        std::uint64_t wakeCredential = 0x1b3d4f5a;
        // Registration
        std::uint32_t pin = 12345678;
        protocol::PsnAccountId account{{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
        bool answerRegistSearch = true;
        /// If set, sent verbatim instead of a real answer. For malformed-input tests.
        std::string rawRegistAnswer;
        // Link simulation for the connection check
        std::chrono::milliseconds answerDelay{0};
        int dropEveryNthSearch = 0; // 0 = answer all
    };

    explicit MockConsole(Config config) : config_(std::move(config)), standby_(config_.standby) {
        discoverySocket_ = openUdp();
        // Registration uses the same port number for UDP and TCP.
        for (int attempt = 0; attempt < 20; ++attempt) {
            registSocket_ = openUdp();
            auto listener = net::TcpListener::listen({net::kLoopbackAddress, registSocket_.localEndpoint().port});
            if (listener) {
                registListener_ = std::move(*listener);
                break;
            }
        }
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    [[nodiscard]] std::uint16_t discoveryPort() const { return discoverySocket_.localEndpoint().port; }
    [[nodiscard]] std::uint16_t registPort() const { return registSocket_.localEndpoint().port; }
    [[nodiscard]] int wakeupsReceived() const { return wakeups_; }
    [[nodiscard]] int searchesReceived() const { return searches_; }
    [[nodiscard]] bool standby() const { return standby_; }

    static std::string registKeyAscii() { return "1b3d4f5a"; }

private:
    static net::UdpSocket openUdp() {
        auto socket = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
        return std::move(socket.value());
    }

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            serveDiscovery();
            serveRegistSearch();
            serveRegistRequest();
        }
    }

    void serveDiscovery() {
        auto datagram = discoverySocket_.receive(std::chrono::milliseconds(5));
        if (!datagram || !datagram->has_value()) return;
        const std::string_view text = asText((*datagram)->data);

        if (text.starts_with("SRCH * HTTP/1.1\n")) {
            ++searches_;
            if (config_.dropEveryNthSearch > 0 && searches_ % config_.dropEveryNthSearch == 0) return;
            if (config_.answerDelay.count() > 0) std::this_thread::sleep_for(config_.answerDelay);
            std::string answer = standby_ ? "HTTP/1.1 620 Server Standby\n" : "HTTP/1.1 200 Ok\n";
            answer += "host-id:" + config_.hostId + "\n";
            answer += "host-type:PS5\n";
            answer += "host-name:" + config_.hostName + "\n";
            answer += "host-request-port:997\n";
            answer += "device-discovery-protocol-version:00030010\n";
            answer += "system-version:09400004\n";
            if (!standby_) answer += "running-app-name:Astro's Playroom\nrunning-app-titleid:PPSA01325\n";
            (void)discoverySocket_.sendTo(asBytes(answer), (*datagram)->from);
        } else if (text.starts_with("WAKEUP * HTTP/1.1\n")) {
            const auto headers = protocol::parseHeaderLines(text.substr(text.find('\n') + 1));
            const auto credential = headers.get("user-credential");
            if (credential && *credential == std::to_string(config_.wakeCredential)) {
                ++wakeups_;
                standby_ = false;
            }
        }
    }

    void serveRegistSearch() {
        auto datagram = registSocket_.receive(std::chrono::milliseconds(5));
        if (!datagram || !datagram->has_value()) return;
        if (!config_.answerRegistSearch) return;
        if (asText((*datagram)->data) == protocol::kRegistSearchRequest) {
            (void)registSocket_.sendTo(asBytes(std::string_view("RES3")), (*datagram)->from);
        }
    }

    void serveRegistRequest() {
        auto accepted = registListener_.accept(std::chrono::milliseconds(5));
        if (!accepted || !accepted->has_value()) return;
        net::TcpSocket client = std::move(**accepted);

        Bytes buffer;
        std::optional<std::size_t> headerEnd;
        std::size_t total = 0;
        while (!headerEnd || buffer.size() < total) {
            std::uint8_t chunk[1024];
            const auto received = client.receiveSome(chunk, std::chrono::milliseconds(2000));
            if (!received || *received == 0) return;
            buffer.insert(buffer.end(), chunk, chunk + *received);
            if (!headerEnd) {
                headerEnd = protocol::findHeaderEnd(buffer);
                if (headerEnd) {
                    const auto headers = protocol::parseHeaderLines(asText(buffer).substr(0, *headerEnd));
                    total = *headerEnd + std::stoul(std::string(headers.get("Content-Length").value_or("0")));
                }
            }
        }
        if (!asText(buffer).starts_with("POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n")) return;

        const std::span<const std::uint8_t> payload(buffer.data() + *headerEnd, total - *headerEnd);
        const std::string answer = config_.rawRegistAnswer.empty() ? answerFor(payload) : config_.rawRegistAnswer;
        (void)client.sendAll(asBytes(answer), std::chrono::milliseconds(2000));
    }

    /// Console side of the exchange: recover the nonce, rebuild the key from
    /// the PIN shown on screen and check that the inner header decrypts.
    std::string answerFor(std::span<const std::uint8_t> payload) {
        const auto reject = [](std::string_view reason) {
            return "HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: " + std::string(reason) + "\r\n\r\n";
        };
        if (payload.size() <= protocol::kRegistPaddingSize) return reject("80108b09");

        const std::size_t keyColumn = payload[0x18d] & 0x1f;
        const std::size_t maskColumn = payload[0] >> 3;
        protocol::RegistNonce masked{};
        std::copy_n(payload.begin() + 0x191, 8, masked.begin());
        std::copy_n(payload.begin() + 0xc7, 8, masked.begin() + 8);
        const auto nonce = protocol::unmaskRegistNonce(masked, maskColumn);
        const auto cipher = protocol::RegistCipher::create(*nonce, keyColumn, config_.pin);

        const auto inner = cipher->decrypt(payload.subspan(protocol::kRegistPaddingSize));
        const auto headers = protocol::parseHeaderLines(asText(*inner));
        // With a wrong PIN the key differs and the header decrypts to noise.
        if (!headers.get("Client-Type") || !headers.get("Np-AccountId")) return reject("80108b09");
        if (*headers.get("Np-AccountId") != config_.account.toBase64()) return reject("80108b02");

        std::string body;
        body += "AP-Ssid: \r\n";
        body += "PS5-Mac: " + toHex(*fromHex(config_.hostId)) + "\r\n";
        body += "PS5-RegistKey: " + toHex(asBytes(registKeyAscii())) + "\r\n";
        body += "PS5-Nickname: " + config_.hostName + "\r\n";
        body += "RP-KeyType: 2\r\n";
        body += "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n";
        const auto encrypted = cipher->encrypt(asBytes(body));

        std::string answer = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(encrypted->size()) + "\r\n\r\n";
        answer.append(asText(*encrypted));
        return answer;
    }

    Config config_;
    std::atomic<bool> standby_;
    std::atomic<int> wakeups_{0};
    std::atomic<int> searches_{0};
    net::UdpSocket discoverySocket_;
    net::UdpSocket registSocket_;
    net::TcpListener registListener_;
    std::jthread thread_;
};

/// Polls `condition` until it holds or `timeout` passes.
template <class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

} // namespace veloplay::test
