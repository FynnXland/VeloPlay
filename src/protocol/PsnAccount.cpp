#include "protocol/PsnAccount.hpp"

#include <charconv>

#include <nlohmann/json.hpp>

#include "util/Bytes.hpp"

namespace veloplay::protocol {

namespace {

using json = nlohmann::json;

constexpr std::string_view kClientId = "ba495a24-818c-472b-b12d-ff231c1b5745";
constexpr std::string_view kClientSecret = "mvaiZkRsAsI1IBkY";
constexpr std::string_view kRedirect = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
constexpr std::string_view kTokenUrl = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";
constexpr std::uint64_t kMaxAnswer = 64 * 1024;

std::string basicAuth() {
    const std::string pair = std::string(kClientId) + ":" + std::string(kClientSecret);
    return "Basic " + base64Encode(asBytes(pair));
}

/// Percent-encodes everything but the characters that are safe anywhere in a URL.
std::string urlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        const bool safe = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '-' ||
                          u == '_' || u == '.' || u == '~';
        if (safe) {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 0x0f];
        }
    }
    return out;
}

std::string urlDecode(std::string_view text) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() + 0 && digit(text[i + 1]) >= 0 && digit(text[i + 2]) >= 0) {
            out += static_cast<char>(digit(text[i + 1]) * 16 + digit(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace

std::string psnLoginUrl() {
    return "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn"
           "&response_type=code&client_id=" +
           std::string(kClientId) + "&redirect_uri=" + std::string(kRedirect) +
           "&scope=psn:clientapp&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay"
           "&prompt=always&PlatformPrivacyWs1=minimal&";
}

std::string_view psnRedirectPrefix() { return kRedirect; }

std::optional<std::string> psnCodeFromRedirect(std::string_view url) {
    url = trim(url);
    if (url.substr(0, kRedirect.size()) != kRedirect) return std::nullopt;
    const std::size_t question = url.find('?');
    if (question == std::string_view::npos) return std::nullopt;
    std::string_view query = url.substr(question + 1);
    if (const std::size_t hash = query.find('#'); hash != std::string_view::npos) query = query.substr(0, hash);

    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        if (pair.substr(0, 5) == "code=") {
            std::string code = urlDecode(pair.substr(5));
            // A code is a short token. Anything else is not from Sony.
            const bool plausible = !code.empty() && code.size() <= 128 &&
                                   code.find_first_of(" \t\r\n&=\"'<>") == std::string::npos;
            return plausible ? std::optional<std::string>(std::move(code)) : std::nullopt;
        }
        if (amp == std::string_view::npos) break;
        query = query.substr(amp + 1);
    }
    return std::nullopt;
}

update::HttpRequest psnTokenRequest(std::string_view code) {
    update::HttpRequest request;
    request.method = "POST";
    request.url = std::string(kTokenUrl);
    request.headers = {{"Authorization", basicAuth()}, {"Content-Type", "application/x-www-form-urlencoded"}};
    request.body = "grant_type=authorization_code&code=" + urlEncode(code) + "&redirect_uri=" + std::string(kRedirect) + "&";
    return request;
}

update::HttpRequest psnAccountRequest(std::string_view accessToken) {
    update::HttpRequest request;
    request.url = std::string(kTokenUrl) + "/" + urlEncode(accessToken);
    request.headers = {{"Authorization", basicAuth()}};
    return request;
}

Result<std::string> psnAccessTokenFromAnswer(std::string_view text) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("access_token") || !root["access_token"].is_string()) {
        return Error{ErrorCode::InvalidData, "the sign-in answer carries no access token"};
    }
    std::string token = root["access_token"].get<std::string>();
    if (token.empty()) return Error{ErrorCode::InvalidData, "the sign-in answer carries an empty access token"};
    return token;
}

Result<PsnAccountId> psnAccountIdFromAnswer(std::string_view text) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("user_id")) {
        return Error{ErrorCode::InvalidData, "the account answer carries no user id"};
    }
    std::string decimal;
    if (root["user_id"].is_string()) decimal = root["user_id"].get<std::string>();
    else if (root["user_id"].is_number_unsigned()) decimal = std::to_string(root["user_id"].get<std::uint64_t>());
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(decimal.data(), decimal.data() + decimal.size(), number);
    if (decimal.empty() || parsed.ec != std::errc{} || parsed.ptr != decimal.data() + decimal.size() || number == 0) {
        return Error{ErrorCode::InvalidData, "the account answer carries an unusable user id"};
    }
    PsnAccountId id;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) id.bytes[i] = static_cast<std::uint8_t>(number >> (8 * i));
    return id;
}

Result<PsnAccountId> lookUpPsnAccount(update::IHttpClient& http, std::string_view code, std::stop_token stop) {
    const auto tokenAnswer = http.send(psnTokenRequest(code), kMaxAnswer, stop);
    if (!tokenAnswer) return tokenAnswer.error();
    const auto token = psnAccessTokenFromAnswer(asText(*tokenAnswer));
    if (!token) return token.error();

    const auto accountAnswer = http.send(psnAccountRequest(*token), kMaxAnswer, stop);
    if (!accountAnswer) return accountAnswer.error();
    return psnAccountIdFromAnswer(asText(*accountAnswer));
}

void PsnAccountLookup::start(std::string code) {
    cancel();
    thread_ = std::jthread([this, code = std::move(code)](std::stop_token stop) {
        auto result = lookUpPsnAccount(http_, code, stop);
        std::lock_guard lock(mutex_);
        result_ = std::move(result);
    });
}

void PsnAccountLookup::cancel() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    result_.reset();
}

bool PsnAccountLookup::finished() const {
    std::lock_guard lock(mutex_);
    return result_.has_value();
}

std::optional<Result<PsnAccountId>> PsnAccountLookup::take() {
    std::optional<Result<PsnAccountId>> out;
    {
        std::lock_guard lock(mutex_);
        out.swap(result_);
    }
    if (out && thread_.joinable()) thread_.join();
    return out;
}

} // namespace veloplay::protocol
