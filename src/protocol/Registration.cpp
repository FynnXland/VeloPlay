#include "protocol/Registration.hpp"

#include <algorithm>
#include <charconv>

#include "protocol/HttpMessage.hpp"
#include "protocol/RegistKeys.hpp"

namespace veloplay::protocol {

namespace {

constexpr std::size_t kColumns = 32;
constexpr std::uint8_t kMaskOffset = 0xd3;

// Offsets inside the padding block.
constexpr std::size_t kKeyColumnOffset = 0x18d;
constexpr std::size_t kMaskColumnOffset = 0x00;
constexpr std::size_t kMaskedNonceHighOffset = 0xc7; // receives masked[8..16)
constexpr std::size_t kMaskedNonceLowOffset = 0x191; // receives masked[0..8)

constexpr std::string_view kClientType = "dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f";
constexpr std::string_view kRegistPath = "/sie/ps5/rp/sess/rgst";
constexpr std::string_view kRpVersion = "1.0";

std::string buildHttpHeader(std::size_t contentLength) {
    // The doubled "HTTP/1.1" line and the fixed HOST value mirror what consoles
    // are known to accept. Do not "fix" them without testing on hardware.
    std::string header;
    header += "POST ";
    header += kRegistPath;
    header += " HTTP/1.1\r\n HTTP/1.1\r\n";
    header += "HOST: 10.0.2.15\r\n";
    header += "User-Agent: remoteplay Windows\r\n";
    header += "Connection: close\r\n";
    header += "Content-Length: " + std::to_string(contentLength) + "\r\n";
    header += "RP-Version: ";
    header += kRpVersion;
    header += "\r\n\r\n";
    return header;
}

} // namespace

std::optional<PsnAccountId> PsnAccountId::parse(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;

    PsnAccountId id;
    const bool allDigits = std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    // Base64 of 8 bytes is 11 or 12 characters, so a longer run of digits can
    // only be the decimal notation.
    if (allDigits && text.size() > 12) {
        std::uint64_t number = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
        for (std::size_t i = 0; i < id.bytes.size(); ++i) id.bytes[i] = static_cast<std::uint8_t>(number >> (8 * i));
        return id;
    }

    const auto decoded = base64Decode(text);
    if (!decoded || decoded->size() != id.bytes.size()) return std::nullopt;
    std::copy(decoded->begin(), decoded->end(), id.bytes.begin());
    return id;
}

std::string PsnAccountId::toBase64() const { return base64Encode(bytes); }

std::uint64_t PsnAccountId::toNumber() const {
    std::uint64_t number = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) number |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return number;
}

std::optional<std::uint32_t> parsePin(std::string_view text) {
    std::uint32_t pin = 0;
    int digits = 0;
    for (char c : text) {
        if (c == ' ' || c == '-') continue;
        if (c < '0' || c > '9') return std::nullopt;
        if (++digits > 8) return std::nullopt;
        pin = pin * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (digits != 8) return std::nullopt;
    return pin;
}

Result<RegistCipher> RegistCipher::create(const RegistNonce& nonce, std::size_t keyColumn, std::uint32_t pin) {
    if (keyColumn >= kColumns) return Error{ErrorCode::InvalidArgument, "key column out of range"};

    RegistCipher cipher;
    cipher.nonce_ = nonce;
    for (std::size_t i = 0; i < cipher.key_.size(); ++i) {
        cipher.key_[i] = regist::kPs5RegistKeys0[i * kColumns + keyColumn];
    }
    cipher.key_[12] ^= static_cast<std::uint8_t>(pin >> 24);
    cipher.key_[13] ^= static_cast<std::uint8_t>(pin >> 16);
    cipher.key_[14] ^= static_cast<std::uint8_t>(pin >> 8);
    cipher.key_[15] ^= static_cast<std::uint8_t>(pin);
    return cipher;
}

Result<crypto::Key128> RegistCipher::iv(std::uint64_t counter) const {
    std::array<std::uint8_t, 24> input{};
    std::copy(nonce_.begin(), nonce_.end(), input.begin());
    for (std::size_t i = 0; i < 8; ++i) {
        input[nonce_.size() + i] = static_cast<std::uint8_t>(counter >> (8 * (7 - i)));
    }

    const auto digest = crypto::hmacSha256(regist::kPs5HmacKey, input);
    if (!digest) return digest.error();

    crypto::Key128 iv{};
    std::copy_n(digest->begin(), iv.size(), iv.begin());
    return iv;
}

Result<Bytes> RegistCipher::encrypt(std::span<const std::uint8_t> plaintext, std::uint64_t counter) const {
    const auto ivValue = iv(counter);
    if (!ivValue) return ivValue.error();
    return crypto::aes128CfbEncrypt(key_, *ivValue, plaintext);
}

Result<Bytes> RegistCipher::decrypt(std::span<const std::uint8_t> ciphertext, std::uint64_t counter) const {
    const auto ivValue = iv(counter);
    if (!ivValue) return ivValue.error();
    return crypto::aes128CfbDecrypt(key_, *ivValue, ciphertext);
}

Result<RegistNonce> maskRegistNonce(const RegistNonce& nonce, std::size_t maskColumn) {
    if (maskColumn >= kColumns) return Error{ErrorCode::InvalidArgument, "mask column out of range"};
    RegistNonce masked{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        const std::uint8_t k = regist::kPs5RegistKeys1[i * kColumns + maskColumn];
        masked[i] = static_cast<std::uint8_t>((nonce[i] ^ k) + kMaskOffset + i);
    }
    return masked;
}

Result<RegistNonce> unmaskRegistNonce(const RegistNonce& masked, std::size_t maskColumn) {
    if (maskColumn >= kColumns) return Error{ErrorCode::InvalidArgument, "mask column out of range"};
    RegistNonce nonce{};
    for (std::size_t i = 0; i < masked.size(); ++i) {
        const std::uint8_t k = regist::kPs5RegistKeys1[i * kColumns + maskColumn];
        nonce[i] = static_cast<std::uint8_t>(masked[i] - kMaskOffset - i) ^ k;
    }
    return nonce;
}

Result<RegistRequest> buildRegistRequest(const PsnAccountId& account, std::uint32_t pin, const RegistNonce& nonce,
                                         std::span<const std::uint8_t> padding) {
    if (padding.size() != kRegistPaddingSize) {
        return Error{ErrorCode::InvalidArgument, "registration padding must be 480 bytes"};
    }
    if (pin > 99'999'999) return Error{ErrorCode::InvalidArgument, "PIN must have 8 digits"};

    Bytes payload(padding.begin(), padding.end());
    const std::size_t keyColumn = payload[kKeyColumnOffset] & 0x1f;
    const std::size_t maskColumn = payload[kMaskColumnOffset] >> 3;

    auto cipher = RegistCipher::create(nonce, keyColumn, pin);
    if (!cipher) return cipher.error();
    const auto masked = maskRegistNonce(nonce, maskColumn);
    if (!masked) return masked.error();

    std::copy_n(masked->begin() + 8, 8, payload.begin() + kMaskedNonceHighOffset);
    std::copy_n(masked->begin(), 8, payload.begin() + kMaskedNonceLowOffset);

    std::string inner;
    inner += "Client-Type: ";
    inner += kClientType;
    inner += "\r\nNp-AccountId: ";
    inner += account.toBase64();
    inner += "\r\n";

    const auto encrypted = cipher->encrypt(asBytes(inner));
    if (!encrypted) return encrypted.error();
    payload.insert(payload.end(), encrypted->begin(), encrypted->end());

    return RegistRequest{buildHttpHeader(payload.size()), std::move(payload), *cipher};
}

Result<RegistRequest> buildRegistRequest(const PsnAccountId& account, std::uint32_t pin) {
    RegistNonce nonce{};
    if (const auto random = crypto::randomBytes(nonce); !random) return random.error();

    // The console reads only the bytes that select the columns and that carry
    // the masked nonce. A constant filler is what existing clients send, so we
    // stay with the variant that is proven on real hardware.
    const Bytes padding(kRegistPaddingSize, 'A');
    return buildRegistRequest(account, pin, nonce, padding);
}

std::string RegisteredHost::hostId() const {
    std::string id = toHex(mac);
    std::transform(id.begin(), id.end(), id.begin(), [](char c) {
        return (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
    });
    return id;
}

Result<RegisteredHost> parseRegistResponse(std::string_view decryptedBody) {
    const HttpHeaders headers = parseHeaderLines(decryptedBody);
    const auto text = [&](std::string_view name) {
        const auto value = headers.get(name);
        return value ? std::string(*value) : std::string{};
    };

    RegisteredHost host;
    host.nickname = text("PS5-Nickname");
    host.apSsid = text("AP-Ssid");
    host.apBssid = text("AP-Bssid");
    host.apKey = text("AP-Key");
    host.apName = text("AP-Name");

    const auto registKeyHex = headers.get("PS5-RegistKey");
    const auto registKeyBytes = registKeyHex ? fromHex(*registKeyHex) : std::nullopt;
    if (!registKeyBytes || registKeyBytes->empty() || registKeyBytes->size() > 16) {
        return Error{ErrorCode::InvalidData, "registration answer has no valid PS5-RegistKey"};
    }
    host.registKey = std::string(asText(*registKeyBytes));
    host.registKey.resize(host.registKey.find('\0') == std::string::npos ? host.registKey.size()
                                                                          : host.registKey.find('\0'));
    if (host.registKey.empty()) {
        return Error{ErrorCode::InvalidData, "registration answer has an empty PS5-RegistKey"};
    }

    const auto rpKeyHex = headers.get("RP-Key");
    const auto rpKeyBytes = rpKeyHex ? fromHex(*rpKeyHex) : std::nullopt;
    if (!rpKeyBytes || rpKeyBytes->size() != host.rpKey.size()) {
        return Error{ErrorCode::InvalidData, "registration answer has no valid RP-Key"};
    }
    std::copy(rpKeyBytes->begin(), rpKeyBytes->end(), host.rpKey.begin());

    const auto macHex = headers.get("PS5-Mac");
    const auto macBytes = macHex ? fromHex(*macHex) : std::nullopt;
    if (!macBytes || macBytes->size() != host.mac.size()) {
        return Error{ErrorCode::InvalidData, "registration answer has no valid PS5-Mac"};
    }
    std::copy(macBytes->begin(), macBytes->end(), host.mac.begin());

    if (const auto keyType = headers.get("RP-KeyType")) {
        std::from_chars(keyType->data(), keyType->data() + keyType->size(), host.rpKeyType);
    }
    return host;
}

std::string describeApplicationReason(std::uint32_t reason) {
    switch (reason) {
        case 0x80108b09: return "registration failed, most likely a wrong PIN";
        case 0x80108b02: return "the PSN account ID does not match an account on this console";
        case 0x80108b10: return "Remote Play is already in use";
        case 0x80108b15: return "Remote Play crashed on the console";
        case 0x80108b11: return "protocol version mismatch";
        default: return "unknown reason";
    }
}

} // namespace veloplay::protocol
