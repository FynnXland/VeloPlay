#include "update/HttpClient.hpp"

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace veloplay::update {

namespace {

std::wstring toWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

struct HandleCloser {
    void operator()(HINTERNET handle) const {
        if (handle) WinHttpCloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

Error networkError(const char* what) {
    return Error{ErrorCode::Network, std::string(what) + " failed with error " + std::to_string(GetLastError())};
}

class WinHttpClient final : public IHttpClient {
public:
    explicit WinHttpClient(std::string userAgent) : userAgent_(toWide(userAgent)) {}

    Result<Bytes> get(const std::string& url, std::uint64_t maxSize, const Progress& progress,
                      std::stop_token stop) override {
        HttpRequest request;
        request.url = url;
        request.headers = {{"Accept", "application/vnd.github+json, application/octet-stream"}};
        return perform(request, maxSize, progress, stop);
    }

    Result<Bytes> send(const HttpRequest& request, std::uint64_t maxSize, std::stop_token stop) override {
        return perform(request, maxSize, nullptr, stop);
    }

private:
    Result<Bytes> perform(const HttpRequest& input, std::uint64_t maxSize, const Progress& progress,
                          std::stop_token stop) {
        const std::wstring wideUrl = toWide(input.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = ARRAYSIZE(host);
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = ARRAYSIZE(path);
        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) return networkError("WinHttpCrackUrl");
        if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
            return Error{ErrorCode::InvalidArgument, "only HTTPS addresses are allowed"};
        }

        // System proxy settings apply, like in a browser.
        const Handle session(WinHttpOpen(userAgent_.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) return networkError("WinHttpOpen");
        WinHttpSetTimeouts(session.get(), 10'000, 10'000, 15'000, 30'000);

        const Handle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
        if (!connection) return networkError("WinHttpConnect");

        const std::wstring method = toWide(input.method);
        const Handle request(WinHttpOpenRequest(connection.get(), method.c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request) return networkError("WinHttpOpenRequest");

        // Redirects are followed, but never from HTTPS down to HTTP.
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        std::wstring headers;
        for (const auto& [name, value] : input.headers) headers += toWide(name + ": " + value + "\r\n");
        // WinHTTP wants a non-const pointer for the body but does not change it.
        void* body = input.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(input.body.data());
        const DWORD bodySize = static_cast<DWORD>(input.body.size());
        if (!WinHttpSendRequest(request.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : static_cast<DWORD>(-1), body, bodySize, bodySize, 0)) {
            return networkError("WinHttpSendRequest");
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) return networkError("WinHttpReceiveResponse");

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) return Error{ErrorCode::Rejected, "the server answered with HTTP " + std::to_string(status)};

        DWORD64 total = 0;
        DWORD totalSize = sizeof(total);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &total, &totalSize, WINHTTP_NO_HEADER_INDEX)) {
            total = 0; // chunked answer, size unknown
        }
        if (total > maxSize) return Error{ErrorCode::InvalidData, "the download is larger than expected"};

        Bytes answer;
        answer.reserve(static_cast<std::size_t>(total));
        std::vector<std::uint8_t> chunk(64 * 1024);
        for (;;) {
            if (stop.stop_requested()) return Error{ErrorCode::Canceled, "the download was cancelled"};
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                return networkError("WinHttpReadData");
            }
            if (read == 0) break;
            if (answer.size() + read > maxSize) return Error{ErrorCode::InvalidData, "the download is larger than expected"};
            answer.insert(answer.end(), chunk.begin(), chunk.begin() + read);
            if (progress) progress(answer.size(), total);
        }
        return answer;
    }

    std::wstring userAgent_;
};

} // namespace

std::unique_ptr<IHttpClient> createWinHttpClient(std::string userAgent) {
    return std::make_unique<WinHttpClient>(std::move(userAgent));
}

} // namespace veloplay::update
