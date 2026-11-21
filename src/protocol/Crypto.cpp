#include "protocol/Crypto.hpp"

#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace veloplay::crypto {

namespace {

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};

Result<Bytes> aes128Cfb(const Key128& key, const Key128& iv, std::span<const std::uint8_t> input, bool encrypt) {
    std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return Error{ErrorCode::Crypto, "EVP_CIPHER_CTX_new failed"};

    if (EVP_CipherInit_ex(ctx.get(), EVP_aes_128_cfb128(), nullptr, key.data(), iv.data(), encrypt ? 1 : 0) != 1) {
        return Error{ErrorCode::Crypto, "AES-128-CFB init failed"};
    }

    Bytes output(input.size() + kAesBlockSize);
    int written = 0;
    if (EVP_CipherUpdate(ctx.get(), output.data(), &written, input.data(), static_cast<int>(input.size())) != 1) {
        return Error{ErrorCode::Crypto, "AES-128-CFB update failed"};
    }
    int finalWritten = 0;
    if (EVP_CipherFinal_ex(ctx.get(), output.data() + written, &finalWritten) != 1) {
        return Error{ErrorCode::Crypto, "AES-128-CFB final failed"};
    }
    output.resize(static_cast<std::size_t>(written + finalWritten));
    if (output.size() != input.size()) return Error{ErrorCode::Crypto, "AES-128-CFB produced an unexpected length"};
    return output;
}

} // namespace

Result<Sha256Digest> hmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    Sha256Digest digest{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), digest.data(), &length) ||
        length != digest.size()) {
        return Error{ErrorCode::Crypto, "HMAC-SHA256 failed"};
    }
    return digest;
}

Result<Sha256Digest> sha256(std::span<const std::uint8_t> data) {
    Sha256Digest digest{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != digest.size()) {
        return Error{ErrorCode::Crypto, "SHA-256 failed"};
    }
    return digest;
}

Result<Bytes> aes128CfbEncrypt(const Key128& key, const Key128& iv, std::span<const std::uint8_t> plaintext) {
    return aes128Cfb(key, iv, plaintext, true);
}

Result<Bytes> aes128CfbDecrypt(const Key128& key, const Key128& iv, std::span<const std::uint8_t> ciphertext) {
    return aes128Cfb(key, iv, ciphertext, false);
}

Result<void> randomBytes(std::span<std::uint8_t> out) {
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        return Error{ErrorCode::Crypto, "RAND_bytes failed"};
    }
    return {};
}

} // namespace veloplay::crypto
