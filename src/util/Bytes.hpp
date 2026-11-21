#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veloplay {

using Bytes = std::vector<std::uint8_t>;

inline std::span<const std::uint8_t> asBytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

inline std::string_view asText(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// Lowercase hex without separators.
std::string toHex(std::span<const std::uint8_t> bytes);

/// Accepts upper and lower case. Fails on odd length or non-hex characters.
std::optional<Bytes> fromHex(std::string_view hex);

std::string base64Encode(std::span<const std::uint8_t> bytes);

/// Standard alphabet. Padding is optional, whitespace is not allowed.
std::optional<Bytes> base64Decode(std::string_view text);

std::string_view trim(std::string_view text);

bool equalsIgnoreCase(std::string_view a, std::string_view b);

} // namespace veloplay
