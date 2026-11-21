#pragma once

#include "app/Config.hpp"

namespace veloplay::test {

/// Reversible scrambling for tests. Real builds use DPAPI.
class FakeProtector final : public ISecretProtector {
public:
    explicit FakeProtector(std::uint8_t key = 0x5a) : key_(key) {}

    Result<Bytes> protect(std::span<const std::uint8_t> plaintext) override {
        Bytes out{'F', 'P', key_};
        for (std::uint8_t b : plaintext) out.push_back(b ^ key_);
        return out;
    }

    Result<Bytes> unprotect(std::span<const std::uint8_t> ciphertext) override {
        // A blob written with another key stands for "another Windows user".
        if (ciphertext.size() < 3 || ciphertext[0] != 'F' || ciphertext[1] != 'P' || ciphertext[2] != key_) {
            return Error{ErrorCode::Crypto, "cannot decrypt"};
        }
        Bytes out;
        for (std::uint8_t b : ciphertext.subspan(3)) out.push_back(b ^ key_);
        return out;
    }

private:
    std::uint8_t key_;
};

} // namespace veloplay::test
