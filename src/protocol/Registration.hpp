#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "protocol/Crypto.hpp"
#include "util/Bytes.hpp"
#include "util/Result.hpp"

// One-time pairing ("registration") with a console. The user opens
// Settings > System > Remote Play > Link Device on the PS5, which shows an
// 8-digit PIN. The client proves knowledge of that PIN and of the PSN account
// ID and receives the long-term keys for wake-up and sessions.
//
// This file holds the message formats and the crypto. RegistrationClient does
// the network exchange. See docs/protocol_notes.md for the wire format.

namespace veloplay::protocol {

inline constexpr std::uint16_t kRegistPort = 9295;
inline constexpr std::size_t kRegistPaddingSize = 0x1e0;
inline constexpr std::string_view kRegistSearchRequest{"SRC3\0", 5};
inline constexpr std::string_view kRegistSearchResponse = "RES3";

/// The numeric ID of a PSN account. Not the online ID (user name).
struct PsnAccountId {
    std::array<std::uint8_t, 8> bytes{};

    /// Accepts both common notations: base64 of the 8 bytes ("mZ3N5jZ2tAA=")
    /// or the decimal number (a 64 bit integer, stored little-endian).
    static std::optional<PsnAccountId> parse(std::string_view text);

    [[nodiscard]] std::string toBase64() const;
    [[nodiscard]] std::uint64_t toNumber() const;

    friend bool operator==(const PsnAccountId&, const PsnAccountId&) = default;
};

/// Parses the 8-digit PIN. Spaces and dashes between digits are ignored,
/// because the console shows the PIN in two groups of four.
std::optional<std::uint32_t> parsePin(std::string_view text);

using RegistNonce = std::array<std::uint8_t, 16>;

/// Encryption context of one registration exchange.
class RegistCipher {
public:
    /// `keyColumn` (0..31) selects the base key; the PIN is mixed into it.
    static Result<RegistCipher> create(const RegistNonce& nonce, std::size_t keyColumn, std::uint32_t pin);

    [[nodiscard]] Result<Bytes> encrypt(std::span<const std::uint8_t> plaintext, std::uint64_t counter = 0) const;
    [[nodiscard]] Result<Bytes> decrypt(std::span<const std::uint8_t> ciphertext, std::uint64_t counter = 0) const;

    /// IV = first 16 bytes of HMAC-SHA256(protocol key, nonce || counter).
    [[nodiscard]] Result<crypto::Key128> iv(std::uint64_t counter) const;

    [[nodiscard]] const crypto::Key128& key() const { return key_; }
    [[nodiscard]] const RegistNonce& nonce() const { return nonce_; }

private:
    crypto::Key128 key_{};
    RegistNonce nonce_{};
};

/// The nonce travels masked inside the request. `maskColumn` is 0..31.
Result<RegistNonce> maskRegistNonce(const RegistNonce& nonce, std::size_t maskColumn);
Result<RegistNonce> unmaskRegistNonce(const RegistNonce& masked, std::size_t maskColumn);

struct RegistRequest {
    std::string httpHeader; // sent first, plain text
    Bytes payload;          // padding with the masked nonce, then the encrypted inner header
    RegistCipher cipher;    // decrypts the console's answer
};

/// Builds a request from explicit inputs. `padding` must be kRegistPaddingSize
/// bytes; two of its bytes select the key and mask columns.
Result<RegistRequest> buildRegistRequest(const PsnAccountId& account, std::uint32_t pin, const RegistNonce& nonce,
                                         std::span<const std::uint8_t> padding);

/// Builds a request with a random nonce and the default padding.
Result<RegistRequest> buildRegistRequest(const PsnAccountId& account, std::uint32_t pin);

/// What the console hands out after a successful pairing.
struct RegisteredHost {
    std::string nickname;
    std::array<std::uint8_t, 6> mac{};
    std::string registKey;   // short ASCII hex string, identifies this client
    crypto::Key128 rpKey{};  // long-term secret for session authentication
    std::uint32_t rpKeyType = 0;
    std::string apSsid;
    std::string apBssid;
    std::string apKey;
    std::string apName;

    /// MAC as 12 uppercase hex digits. Matches "host-id" in discovery answers.
    [[nodiscard]] std::string hostId() const;

    friend bool operator==(const RegisteredHost&, const RegisteredHost&) = default;
};

/// Parses the decrypted body of a successful registration answer.
Result<RegisteredHost> parseRegistResponse(std::string_view decryptedBody);

/// Human-readable meaning of the "RP-Application-Reason" error code.
std::string describeApplicationReason(std::uint32_t reason);

} // namespace veloplay::protocol
