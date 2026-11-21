#pragma once

#include <vector>
#include <utility>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

#include "util/Bytes.hpp"
#include "util/Result.hpp"

namespace veloplay::update {

/// A request with a method, headers and a body, for the few calls that are
/// more than a plain download.
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

/// The little bit of HTTPS the updater needs. Windows builds use WinHTTP;
/// tests answer from memory.
class IHttpClient {
public:
    /// Called with bytes received so far and the total size (0 if unknown).
    using Progress = std::function<void(std::uint64_t received, std::uint64_t total)>;

    virtual ~IHttpClient() = default;

    /// Fetches `url` with GET. Fails on anything but HTTPS, on a status other
    /// than 200 and on bodies larger than `maxSize`. Error codes: Network,
    /// Rejected (HTTP status, in the message), InvalidData (too large), Canceled.
    virtual Result<Bytes> get(const std::string& url, std::uint64_t maxSize, const Progress& progress,
                              std::stop_token stop) = 0;

    /// Sends any request. Same rules and error codes as get().
    virtual Result<Bytes> send(const HttpRequest& request, std::uint64_t maxSize, std::stop_token stop) {
        (void)request;
        (void)maxSize;
        (void)stop;
        return Error{ErrorCode::InvalidArgument, "this HTTP client only downloads"};
    }
};

#ifdef _WIN32
/// `userAgent` identifies the client to the server, for example "VeloPlay/0.2.0".
std::unique_ptr<IHttpClient> createWinHttpClient(std::string userAgent);
#endif

} // namespace veloplay::update
