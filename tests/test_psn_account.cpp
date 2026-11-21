#include <gtest/gtest.h>

#include <thread>

#include "MockConsole.hpp"
#include "protocol/PsnAccount.hpp"

using namespace veloplay;
using namespace veloplay::protocol;
using namespace std::chrono_literals;

namespace {

/// Plays Sony's two answers and keeps what was asked.
struct FakeSony final : update::IHttpClient {
    std::vector<update::HttpRequest> requests;
    std::string tokenAnswer = R"({"access_token":"tok/en+1","token_type":"bearer","expires_in":3599})";
    std::string accountAnswer = R"({"scopes":"psn:clientapp","user_id":"8104513492015792419","client_id":"x"})";
    std::optional<Error> failToken;
    std::chrono::milliseconds delay{0};

    Result<Bytes> get(const std::string&, std::uint64_t, const Progress&, std::stop_token) override {
        return Error{ErrorCode::InvalidArgument, "not used"};
    }
    Result<Bytes> send(const update::HttpRequest& request, std::uint64_t, std::stop_token stop) override {
        requests.push_back(request);
        std::this_thread::sleep_for(delay);
        if (stop.stop_requested()) return Error{ErrorCode::Canceled, "cancelled"};
        if (request.method == "POST") {
            if (failToken) return *failToken;
            return Bytes(tokenAnswer.begin(), tokenAnswer.end());
        }
        return Bytes(accountAnswer.begin(), accountAnswer.end());
    }
};

std::string header(const update::HttpRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) return value;
    }
    return {};
}

} // namespace

TEST(PsnAccount, BuildsTheSignInAddress) {
    const std::string url = psnLoginUrl();
    EXPECT_EQ(url.rfind("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?", 0), 0u);
    EXPECT_NE(url.find("client_id=ba495a24-818c-472b-b12d-ff231c1b5745"), std::string::npos);
    EXPECT_NE(url.find("redirect_uri=https://remoteplay.dl.playstation.net/remoteplay/redirect"), std::string::npos);
    EXPECT_NE(url.find("scope=psn:clientapp"), std::string::npos);
    EXPECT_NE(url.find("response_type=code"), std::string::npos);
}

TEST(PsnAccount, TakesTheCodeFromTheRedirectOnly) {
    const std::string base = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
    EXPECT_EQ(psnCodeFromRedirect(base + "?code=AbC123&cid=00000000-0000"), "AbC123");
    EXPECT_EQ(psnCodeFromRedirect("  " + base + "?cid=1&code=x%2Dy#frag \n"), "x-y");

    EXPECT_FALSE(psnCodeFromRedirect(base).has_value());
    EXPECT_FALSE(psnCodeFromRedirect(base + "?cid=1").has_value());
    EXPECT_FALSE(psnCodeFromRedirect(base + "?code=").has_value());
    EXPECT_FALSE(psnCodeFromRedirect(base + "?error=access_denied").has_value());
    // Another site that happens to carry a code is not Sony's answer.
    EXPECT_FALSE(psnCodeFromRedirect("https://example.com/remoteplay/redirect?code=AbC123").has_value());
    EXPECT_FALSE(psnCodeFromRedirect("ASNFZ4mrze8=").has_value());
    EXPECT_FALSE(psnCodeFromRedirect(base + "?code=" + std::string(500, 'a')).has_value());
    EXPECT_FALSE(psnCodeFromRedirect(base + "?code=a%20b").has_value());
}

TEST(PsnAccount, ReadsSonysAnswers) {
    EXPECT_EQ(*psnAccessTokenFromAnswer(R"({"access_token":"abc"})"), "abc");
    EXPECT_FALSE(psnAccessTokenFromAnswer(R"({"error":"invalid_grant"})").ok());
    EXPECT_FALSE(psnAccessTokenFromAnswer(R"({"access_token":""})").ok());
    EXPECT_FALSE(psnAccessTokenFromAnswer("<html>").ok());

    // 8104513492015792419 = 0x7079_1122_3344_5523, stored lowest byte first.
    const auto id = psnAccountIdFromAnswer(R"({"user_id":"8104513492015792419"})");
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id->toNumber(), 8104513492015792419ull);
    EXPECT_EQ(id->bytes[0], 8104513492015792419ull & 0xff);
    EXPECT_EQ(id->toBase64(), PsnAccountId::parse("8104513492015792419")->toBase64());
    EXPECT_TRUE(psnAccountIdFromAnswer(R"({"user_id":12345})").ok());

    EXPECT_FALSE(psnAccountIdFromAnswer(R"({"scopes":"x"})").ok());
    EXPECT_FALSE(psnAccountIdFromAnswer(R"({"user_id":"abc"})").ok());
    EXPECT_FALSE(psnAccountIdFromAnswer(R"({"user_id":""})").ok());
    EXPECT_FALSE(psnAccountIdFromAnswer("").ok());
}

TEST(PsnAccount, AsksSonyInTwoSteps) {
    FakeSony sony;
    const auto id = lookUpPsnAccount(sony, "the-code", {});
    ASSERT_TRUE(id.ok()) << id.error().message;
    EXPECT_EQ(id->toNumber(), 8104513492015792419ull);

    ASSERT_EQ(sony.requests.size(), 2u);
    const update::HttpRequest& token = sony.requests[0];
    EXPECT_EQ(token.method, "POST");
    EXPECT_EQ(token.url, "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token");
    EXPECT_EQ(header(token, "Content-Type"), "application/x-www-form-urlencoded");
    // Client id and secret of the Remote Play client, as HTTP basic authentication.
    EXPECT_EQ(header(token, "Authorization"),
              "Basic YmE0OTVhMjQtODE4Yy00NzJiLWIxMmQtZmYyMzFjMWI1NzQ1Om12YWlaa1JzQXNJMUlCa1k=");
    EXPECT_EQ(token.body, "grant_type=authorization_code&code=the-code"
                          "&redirect_uri=https://remoteplay.dl.playstation.net/remoteplay/redirect&");

    const update::HttpRequest& account = sony.requests[1];
    EXPECT_EQ(account.method, "GET");
    // The token becomes part of the path, so it has to be escaped.
    EXPECT_EQ(account.url, "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/tok%2Fen%2B1");
    EXPECT_FALSE(header(account, "Authorization").empty());
}

TEST(PsnAccount, PassesFailuresOn) {
    FakeSony refused;
    refused.failToken = Error{ErrorCode::Rejected, "the server answered with HTTP 400"};
    const auto first = lookUpPsnAccount(refused, "used-twice", {});
    ASSERT_FALSE(first.ok());
    EXPECT_EQ(first.error().code, ErrorCode::Rejected);
    EXPECT_EQ(refused.requests.size(), 1u); // no second request without a token

    FakeSony odd;
    odd.accountAnswer = R"({"error":"invalid_token"})";
    EXPECT_EQ(lookUpPsnAccount(odd, "code", {}).error().code, ErrorCode::InvalidData);
}

TEST(PsnAccountLookup, RunsInTheBackground) {
    FakeSony sony;
    sony.delay = 30ms;
    PsnAccountLookup lookup(sony);
    EXPECT_FALSE(lookup.take().has_value());

    lookup.start("code");
    EXPECT_TRUE(lookup.running());
    std::optional<Result<PsnAccountId>> result;
    ASSERT_TRUE(test::waitFor([&] {
        result = lookup.take();
        return result.has_value();
    }));
    ASSERT_TRUE(result->ok());
    EXPECT_EQ((*result)->toNumber(), 8104513492015792419ull);
    EXPECT_FALSE(lookup.running());
    EXPECT_FALSE(lookup.take().has_value()); // handed out once

    // Cancelling drops whatever was under way.
    lookup.start("code");
    lookup.cancel();
    EXPECT_FALSE(lookup.running());
    EXPECT_FALSE(lookup.take().has_value());
}
