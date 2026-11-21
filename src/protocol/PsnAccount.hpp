#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "protocol/Registration.hpp"
#include "update/HttpClient.hpp"
#include "util/Result.hpp"

// Finds out the account ID a console wants for pairing, by letting the user
// sign in at Sony and asking Sony who signed in.
//
// The procedure, the addresses and the client identification are those of
// psn-account-id.py from chiaki-ng (https://github.com/streetpea/chiaki-ng,
// AGPL-3.0), which credits grill2010 for working it out. The identification
// is the one of Sony's own Remote Play client.

namespace veloplay::protocol {

/// Page to open in a browser. After signing in, Sony sends the browser to an
/// address that starts with psnRedirectPrefix() and carries a one-time code.
std::string psnLoginUrl();
std::string_view psnRedirectPrefix();

/// The one-time code from the address the browser was sent to.
std::optional<std::string> psnCodeFromRedirect(std::string_view url);

/// The two requests that turn the code into the account ID.
update::HttpRequest psnTokenRequest(std::string_view code);
update::HttpRequest psnAccountRequest(std::string_view accessToken);

Result<std::string> psnAccessTokenFromAnswer(std::string_view json);
/// The answer carries the account as a decimal number; pairing wants its
/// eight bytes, lowest first.
Result<PsnAccountId> psnAccountIdFromAnswer(std::string_view json);

/// Runs both requests. Blocking; nothing of the answers is logged or kept.
Result<PsnAccountId> lookUpPsnAccount(update::IHttpClient& http, std::string_view code, std::stop_token stop);

/// The same in the background, for a caller that polls.
class PsnAccountLookup {
public:
    explicit PsnAccountLookup(update::IHttpClient& http) : http_(http) {}
    ~PsnAccountLookup() { cancel(); }

    void start(std::string code);
    void cancel();
    [[nodiscard]] bool running() const { return thread_.joinable() && !finished(); }

    /// The outcome, once. Empty while it runs or when nothing was started.
    std::optional<Result<PsnAccountId>> take();

private:
    [[nodiscard]] bool finished() const;

    update::IHttpClient& http_;
    std::jthread thread_;
    mutable std::mutex mutex_;
    std::optional<Result<PsnAccountId>> result_;
};

} // namespace veloplay::protocol
