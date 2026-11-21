#include <gtest/gtest.h>

#include <algorithm>

#include "protocol/Crypto.hpp"

using namespace veloplay;

namespace {

crypto::Key128 key128(std::string_view hex) {
    crypto::Key128 key{};
    const auto bytes = fromHex(hex);
    std::copy(bytes->begin(), bytes->end(), key.begin());
    return key;
}

} // namespace

// RFC 4231, test case 2.
TEST(Crypto, HmacSha256MatchesRfc4231) {
    const auto digest = crypto::hmacSha256(asBytes("Jefe"), asBytes("what do ya want for nothing?"));
    ASSERT_TRUE(digest);
    EXPECT_EQ(toHex(*digest), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// NIST SP 800-38A, F.3.13 (CFB128-AES128.Encrypt), first two blocks.
TEST(Crypto, Aes128CfbMatchesNistVector) {
    const auto key = key128("2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv = key128("000102030405060708090a0b0c0d0e0f");
    const auto plaintext = fromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");

    const auto ciphertext = crypto::aes128CfbEncrypt(key, iv, *plaintext);
    ASSERT_TRUE(ciphertext);
    EXPECT_EQ(toHex(*ciphertext), "3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b");

    const auto decrypted = crypto::aes128CfbDecrypt(key, iv, *ciphertext);
    ASSERT_TRUE(decrypted);
    EXPECT_EQ(*decrypted, *plaintext);
}

TEST(Crypto, Aes128CfbKeepsLengthForPartialBlocks) {
    const auto key = key128("2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv = key128("000102030405060708090a0b0c0d0e0f");
    for (std::size_t length : {0u, 1u, 15u, 17u, 100u}) {
        const Bytes plaintext(length, 0x5a);
        const auto ciphertext = crypto::aes128CfbEncrypt(key, iv, plaintext);
        ASSERT_TRUE(ciphertext);
        EXPECT_EQ(ciphertext->size(), length);
        EXPECT_EQ(*crypto::aes128CfbDecrypt(key, iv, *ciphertext), plaintext);
    }
}

TEST(Crypto, RandomBytesAreNotConstant) {
    std::array<std::uint8_t, 32> a{}, b{};
    ASSERT_TRUE(crypto::randomBytes(a));
    ASSERT_TRUE(crypto::randomBytes(b));
    EXPECT_NE(a, b);
}
