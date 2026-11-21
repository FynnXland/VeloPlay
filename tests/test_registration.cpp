#include <gtest/gtest.h>

#include <algorithm>

#include "MockConsole.hpp"
#include "protocol/Registration.hpp"
#include "protocol/RegistrationClient.hpp"
#include "reference_vectors.hpp"

using namespace veloplay;
using namespace veloplay::protocol;
using namespace std::chrono_literals;

namespace {

RegistNonce referenceNonce() {
    RegistNonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<std::uint8_t>(0x10 + i * 7);
    return nonce;
}

RegistNonce nonceFromHex(std::string_view hex) {
    RegistNonce nonce{};
    const auto bytes = fromHex(hex);
    std::copy(bytes->begin(), bytes->end(), nonce.begin());
    return nonce;
}

const PsnAccountId kReferenceAccount{{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
constexpr std::uint32_t kReferencePin = 12345678;

} // namespace

// ---- input parsing -----------------------------------------------------

TEST(PsnAccountId, ParsesBase64) {
    const auto id = PsnAccountId::parse("ASNFZ4mrze8=");
    ASSERT_TRUE(id);
    EXPECT_EQ(*id, kReferenceAccount);
    EXPECT_EQ(id->toBase64(), "ASNFZ4mrze8=");
    EXPECT_EQ(PsnAccountId::parse("  ASNFZ4mrze8  "), kReferenceAccount); // padding and spaces are optional
}

TEST(PsnAccountId, ParsesDecimalAsLittleEndian) {
    const auto id = PsnAccountId::parse("1234567890123456789");
    ASSERT_TRUE(id);
    EXPECT_EQ(id->toNumber(), 1234567890123456789ull);
    EXPECT_EQ(id->bytes[0], 0x15); // 0x112210F47DE98115, lowest byte first
    EXPECT_EQ(id->bytes[7], 0x11);
    EXPECT_EQ(PsnAccountId::parse(id->toBase64()), *id);
}

TEST(PsnAccountId, RejectsWrongInput) {
    EXPECT_FALSE(PsnAccountId::parse(""));
    EXPECT_FALSE(PsnAccountId::parse("my-online-id"));
    EXPECT_FALSE(PsnAccountId::parse("AAAA"));                   // too short
    EXPECT_FALSE(PsnAccountId::parse("ASNFZ4mrze8BAQ=="));       // too long
    EXPECT_FALSE(PsnAccountId::parse("99999999999999999999999")); // does not fit 64 bit
}

TEST(Pin, AcceptsEightDigitsWithSeparators) {
    EXPECT_EQ(parsePin("12345678"), 12345678u);
    EXPECT_EQ(parsePin("1234 5678"), 12345678u);
    EXPECT_EQ(parsePin("1234-5678"), 12345678u);
    EXPECT_EQ(parsePin("00000042"), 42u);
}

TEST(Pin, RejectsEverythingElse) {
    EXPECT_FALSE(parsePin(""));
    EXPECT_FALSE(parsePin("1234567"));
    EXPECT_FALSE(parsePin("123456789"));
    EXPECT_FALSE(parsePin("1234567a"));
    EXPECT_FALSE(parsePin("1234.5678"));
}

// ---- crypto against the reference implementation -------------------------

TEST(RegistCipher, KeyAndMaskedNonceMatchReference) {
    for (const auto& vector : reference::kColumnVectors) {
        const auto cipher = RegistCipher::create(referenceNonce(), vector.keyColumn, kReferencePin);
        ASSERT_TRUE(cipher);
        EXPECT_EQ(toHex(cipher->key()), vector.key) << "key column " << vector.keyColumn;

        const auto masked = maskRegistNonce(referenceNonce(), vector.maskColumn);
        ASSERT_TRUE(masked);
        EXPECT_EQ(toHex(*masked), vector.maskedNonce) << "mask column " << vector.maskColumn;
        EXPECT_EQ(unmaskRegistNonce(*masked, vector.maskColumn).value(), referenceNonce());
    }
}

TEST(RegistCipher, IvMatchesReference) {
    const auto cipher = RegistCipher::create(referenceNonce(), 0, kReferencePin);
    ASSERT_TRUE(cipher);
    EXPECT_EQ(toHex(cipher->iv(0).value()), reference::kIvCounter0);
    EXPECT_NE(toHex(cipher->iv(1).value()), reference::kIvCounter0);
}

// Vectors from chiaki-ng's own unit tests (test/rpcrypt.c).
TEST(RegistCipher, MatchesChiakiUnitTestVectors) {
    const auto ivCipher = RegistCipher::create(nonceFromHex("3e7e7a825973adab2f694346bd44dab5"), 0, 0);
    ASSERT_TRUE(ivCipher);
    EXPECT_EQ(toHex(ivCipher->iv(0).value()), "9044408273f8044dca767b5a16394d64");

    const auto keyCipher = RegistCipher::create(nonceFromHex("dca1c84dfe50d65722da09654231e7c2"), 0x1e, 78703893);
    ASSERT_TRUE(keyCipher);
    EXPECT_EQ(toHex(keyCipher->key()), "e29d644c141b9d617431a56d34cfc17f");
}

TEST(RegistCipher, RejectsColumnsOutOfRange) {
    EXPECT_FALSE(RegistCipher::create(referenceNonce(), 32, 0));
    EXPECT_FALSE(maskRegistNonce(referenceNonce(), 32));
    EXPECT_FALSE(unmaskRegistNonce(referenceNonce(), 32));
}

TEST(RegistRequest, PayloadMatchesReferenceByteForByte) {
    const Bytes padding(kRegistPaddingSize, 'A');
    const auto request = buildRegistRequest(kReferenceAccount, kReferencePin, referenceNonce(), padding);
    ASSERT_TRUE(request);
    EXPECT_EQ(toHex(request->payload), reference::kRequestPayload);
}

TEST(RegistRequest, HttpHeaderMatchesWireFormat) {
    const Bytes padding(kRegistPaddingSize, 'A');
    const auto request = buildRegistRequest(kReferenceAccount, kReferencePin, referenceNonce(), padding);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->httpHeader, "POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n HTTP/1.1\r\n"
                                   "HOST: 10.0.2.15\r\n"
                                   "User-Agent: remoteplay Windows\r\n"
                                   "Connection: close\r\n"
                                   "Content-Length: 587\r\n"
                                   "RP-Version: 1.0\r\n\r\n");
}

TEST(RegistRequest, PaddingBytesSelectTheColumns) {
    Bytes padding(kRegistPaddingSize, 0);
    padding[0x18d] = 0xfa; // key column = 0xfa & 0x1f = 26
    padding[0x00] = 0x2f;  // mask column = 0x2f >> 3 = 5
    const auto request = buildRegistRequest(kReferenceAccount, kReferencePin, referenceNonce(), padding);
    ASSERT_TRUE(request);

    const auto& vector = reference::kColumnVectors[2];
    ASSERT_EQ(vector.keyColumn, 26);
    ASSERT_EQ(vector.maskColumn, 5);
    EXPECT_EQ(toHex(request->cipher.key()), vector.key);

    const std::string masked(vector.maskedNonce);
    const std::string payloadHex = toHex(request->payload);
    EXPECT_EQ(payloadHex.substr(0x191 * 2, 16), masked.substr(0, 16));
    EXPECT_EQ(payloadHex.substr(0xc7 * 2, 16), masked.substr(16, 16));
}

TEST(RegistRequest, ValidatesArguments) {
    const Bytes shortPadding(100, 'A');
    EXPECT_FALSE(buildRegistRequest(kReferenceAccount, kReferencePin, referenceNonce(), shortPadding));
    const Bytes padding(kRegistPaddingSize, 'A');
    EXPECT_FALSE(buildRegistRequest(kReferenceAccount, 123456789, referenceNonce(), padding));
}

TEST(RegistRequest, RandomNonceDiffersBetweenRequests) {
    const auto a = buildRegistRequest(kReferenceAccount, kReferencePin);
    const auto b = buildRegistRequest(kReferenceAccount, kReferencePin);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_NE(a->cipher.nonce(), b->cipher.nonce());
    EXPECT_NE(a->payload, b->payload);
}

TEST(RegistResponse, DecryptsAndParsesReferenceAnswer) {
    const Bytes padding(kRegistPaddingSize, 'A');
    const auto request = buildRegistRequest(kReferenceAccount, kReferencePin, referenceNonce(), padding);
    ASSERT_TRUE(request);

    const auto plain = request->cipher.decrypt(*fromHex(reference::kResponseEncrypted));
    ASSERT_TRUE(plain);
    EXPECT_EQ(asText(*plain), reference::kResponsePlain);

    const auto host = parseRegistResponse(asText(*plain));
    ASSERT_TRUE(host);
    EXPECT_EQ(host->nickname, "PS5-042");
    EXPECT_EQ(host->hostId(), "001122AABBCC");
    EXPECT_EQ(host->registKey, "1b3d4f5a");
    EXPECT_EQ(host->rpKeyType, 2u);
    EXPECT_EQ(toHex(host->rpKey), "000102030405060708090a0b0c0d0e0f");
    EXPECT_TRUE(host->apSsid.empty());
}

TEST(RegistResponse, RegistKeyStopsAtNulPadding) {
    const auto host = parseRegistResponse("PS5-Mac: 001122aabbcc\r\n"
                                          "PS5-RegistKey: 31623364346635610000000000000000\r\n"
                                          "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n");
    ASSERT_TRUE(host);
    EXPECT_EQ(host->registKey, "1b3d4f5a");
}

TEST(RegistResponse, RejectsIncompleteAnswers) {
    const std::string mac = "PS5-Mac: 001122aabbcc\r\n";
    const std::string key = "PS5-RegistKey: 3162336434663561\r\n";
    const std::string rp = "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n";
    EXPECT_FALSE(parseRegistResponse(mac + key));
    EXPECT_FALSE(parseRegistResponse(mac + rp));
    EXPECT_FALSE(parseRegistResponse(key + rp));
    EXPECT_FALSE(parseRegistResponse(mac + key + "RP-Key: 0001\r\n"));
    EXPECT_FALSE(parseRegistResponse("PS5-Mac: zz1122aabbcc\r\n" + key + rp));
    EXPECT_FALSE(parseRegistResponse("random noise \x01\x02"));
}

// ---- full exchange against the mock console ------------------------------

namespace {

RegistrationParams paramsFor(const test::MockConsole& console) {
    RegistrationParams params;
    params.address = net::kLoopbackAddress;
    params.port = console.registPort();
    params.account = kReferenceAccount;
    params.pin = kReferencePin;
    params.searchTimeout = 1500ms;
    params.responseTimeout = 2000ms;
    return params;
}

} // namespace

TEST(RegistrationClient, PairsWithConsole) {
    test::MockConsole console({});
    const auto host = registerWithConsole(paramsFor(console));
    ASSERT_TRUE(host) << host.error().message;
    EXPECT_EQ(host->nickname, "PS5-042");
    EXPECT_EQ(host->hostId(), "001122AABBCC");
    EXPECT_EQ(host->registKey, test::MockConsole::registKeyAscii());
    EXPECT_EQ(toHex(host->rpKey), "000102030405060708090a0b0c0d0e0f");
}

TEST(RegistrationClient, ReportsWrongPin) {
    test::MockConsole console({});
    auto params = paramsFor(console);
    params.pin = 87654321;
    const auto host = registerWithConsole(params);
    ASSERT_FALSE(host);
    EXPECT_EQ(host.error().code, ErrorCode::Rejected);
    EXPECT_NE(host.error().message.find("PIN"), std::string::npos);
}

TEST(RegistrationClient, ReportsWrongAccount) {
    test::MockConsole console({});
    auto params = paramsFor(console);
    params.account.bytes[0] ^= 0xff;
    const auto host = registerWithConsole(params);
    ASSERT_FALSE(host);
    EXPECT_EQ(host.error().code, ErrorCode::Rejected);
    EXPECT_NE(host.error().message.find("account"), std::string::npos);
}

// A console is trusted hardware, but anything on the network can claim to be one.
TEST(RegistrationClient, RejectsMalformedAnswers) {
    const char* answers[] = {
        "garbage without any structure\r\n\r\n",
        "HTTP/1.1 200 OK\r\n\r\n",                                              // no Content-Length
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615\r\n\r\nxx", // would overflow a size sum
        "HTTP/1.1 200 OK\r\nContent-Length: 16\r\n\r\n0123456789abcdef",      // decrypts to noise
    };
    for (const char* answer : answers) {
        test::MockConsole::Config config;
        config.rawRegistAnswer = answer;
        test::MockConsole console(config);
        const auto host = registerWithConsole(paramsFor(console));
        ASSERT_FALSE(host) << answer;
        EXPECT_EQ(host.error().code, ErrorCode::InvalidData) << answer;
    }
}

TEST(RegistrationClient, TimesOutWhenLinkScreenIsClosed) {
    test::MockConsole::Config config;
    config.answerRegistSearch = false;
    test::MockConsole console(config);
    auto params = paramsFor(console);
    params.searchTimeout = 300ms;
    const auto host = registerWithConsole(params);
    ASSERT_FALSE(host);
    EXPECT_EQ(host.error().code, ErrorCode::Timeout);
}

TEST(RegistrationClient, CanBeCancelled) {
    test::MockConsole::Config config;
    config.answerRegistSearch = false;
    test::MockConsole console(config);
    auto params = paramsFor(console);
    params.searchTimeout = 10s;

    std::stop_source stop;
    std::jthread canceller([&] {
        std::this_thread::sleep_for(150ms);
        stop.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto host = registerWithConsole(params, stop.get_token());
    ASSERT_FALSE(host);
    EXPECT_EQ(host.error().code, ErrorCode::Canceled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}
