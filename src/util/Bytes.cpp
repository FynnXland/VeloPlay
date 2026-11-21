#include "util/Bytes.hpp"

#include <array>

namespace veloplay {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace

std::string toHex(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHexDigits[b >> 4]);
        out.push_back(kHexDigits[b & 0x0f]);
    }
    return out;
}

std::optional<Bytes> fromHex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexValue(hex[i]);
        const int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.push_back(kBase64Alphabet[(n >> 6) & 63]);
        out.push_back(kBase64Alphabet[n & 63]);
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t n = bytes[i] << 16;
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.append("==");
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.push_back(kBase64Alphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::optional<Bytes> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    if (text.size() % 4 == 1) return std::nullopt;

    Bytes out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        const int v = base64Value(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

std::string_view trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

} // namespace veloplay
