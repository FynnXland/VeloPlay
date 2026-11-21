#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "util/Bytes.hpp"
#include "util/Result.hpp"

// Small wrappers around OpenSSL. Only what the implemented protocol steps need;
// ECDH and AES-GCM arrive together with the session handshake.

namespace veloplay::crypto {

inline constexpr std::size_t kAesBlockSize = 16;

using Key128 = std::array<std::uint8_t, 16>;
using Sha256Digest = std::array<std::uint8_t, 32>;

Result<Sha256Digest> hmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);

Result<Sha256Digest> sha256(std::span<const std::uint8_t> data);

/// AES-128 in CFB mode with 128 bit feedback. Output has the same length as input.
Result<Bytes> aes128CfbEncrypt(const Key128& key, const Key128& iv, std::span<const std::uint8_t> plaintext);
Result<Bytes> aes128CfbDecrypt(const Key128& key, const Key128& iv, std::span<const std::uint8_t> ciphertext);

/// Cryptographically secure random bytes.
Result<void> randomBytes(std::span<std::uint8_t> out);

} // namespace veloplay::crypto
