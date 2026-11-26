#include "linux/LinuxServices.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#include <curl/curl.h>

namespace veloplay::linuxos {

namespace {

bool isFile(const char* path) {
    std::error_code ignored;
    return path && *path && std::filesystem::is_regular_file(path, ignored);
}

bool isFolder(const char* path) {
    std::error_code ignored;
    return path && *path && std::filesystem::is_directory(path, ignored);
}

bool startsWithHttps(const std::string& url) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() < kScheme.size()) return false;
    return std::equal(kScheme.begin(), kScheme.end(), url.begin(),
                      [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

/// curl_global_init is only safe once, before any other thread uses curl.
void initialiseCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct EasyCloser {
    void operator()(CURL* curl) const { curl_easy_cleanup(curl); }
};
struct ListCloser {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

/// What one transfer collects, and why it ended early if it did.
struct Transfer {
    enum class Abort { None, Rejected, TooLarge, Canceled };

    CURL* curl = nullptr;
    std::uint64_t maxSize = 0;
    const update::IHttpClient::Progress* progress = nullptr;
    std::stop_token stop;
    Bytes body;
    bool headChecked = false;
    long status = 0;
    std::uint64_t total = 0;
    Abort abort = Abort::None;
};

/// Status before size, as WinHTTP does it: a 404 with a large page is a 404.
bool checkHead(Transfer& transfer) {
    if (transfer.headChecked) return true;
    transfer.headChecked = true;
    curl_easy_getinfo(transfer.curl, CURLINFO_RESPONSE_CODE, &transfer.status);
    if (transfer.status != 200) {
        transfer.abort = Transfer::Abort::Rejected;
        return false;
    }
    curl_off_t length = -1;
    if (curl_easy_getinfo(transfer.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length > 0) {
        transfer.total = static_cast<std::uint64_t>(length);
    }
    if (transfer.total > transfer.maxSize) {
        transfer.abort = Transfer::Abort::TooLarge;
        return false;
    }
    return true;
}

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    if (!checkHead(transfer)) return 0; // anything but the number of bytes makes curl stop
    if (transfer.stop.stop_requested()) {
        transfer.abort = Transfer::Abort::Canceled;
        return 0;
    }
    if (transfer.body.size() + bytes > transfer.maxSize) {
        transfer.abort = Transfer::Abort::TooLarge;
        return 0;
    }
    transfer.body.insert(transfer.body.end(), reinterpret_cast<const std::uint8_t*>(data),
                         reinterpret_cast<const std::uint8_t*>(data) + bytes);
    if (transfer.progress && *transfer.progress) (*transfer.progress)(transfer.body.size(), transfer.total);
    return bytes;
}

/// Called by curl about once a second even while nothing arrives, so a stop
/// request ends a transfer that waits on a silent server.
int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& transfer = *static_cast<Transfer*>(user);
    if (transfer.stop.stop_requested()) {
        transfer.abort = Transfer::Abort::Canceled;
        return 1;
    }
    return 0;
}

Error rejected(long status) {
    return Error{ErrorCode::Rejected, "the server answered with HTTP " + std::to_string(status)};
}

class CurlHttpClient final : public update::IHttpClient {
public:
    CurlHttpClient(std::string userAgent, CurlSettings settings)
        : userAgent_(std::move(userAgent)), settings_(std::move(settings)) {
        initialiseCurl();
    }

    Result<Bytes> get(const std::string& url, std::uint64_t maxSize, const Progress& progress,
                      std::stop_token stop) override {
        update::HttpRequest request;
        request.url = url;
        request.headers = {{"Accept", "application/vnd.github+json, application/octet-stream"}};
        return perform(request, maxSize, &progress, std::move(stop));
    }

    Result<Bytes> send(const update::HttpRequest& request, std::uint64_t maxSize, std::stop_token stop) override {
        return perform(request, maxSize, nullptr, std::move(stop));
    }

private:
    Result<Bytes> perform(const update::HttpRequest& request, std::uint64_t maxSize, const Progress* progress,
                          std::stop_token stop) {
        if (!startsWithHttps(request.url)) return Error{ErrorCode::InvalidArgument, "only HTTPS addresses are allowed"};
        if (stop.stop_requested()) return Error{ErrorCode::Canceled, "the download was cancelled"};

        const std::unique_ptr<CURL, EasyCloser> handle(curl_easy_init());
        if (!handle) return Error{ErrorCode::Network, "curl_easy_init failed"};
        CURL* curl = handle.get();

        Transfer transfer;
        transfer.curl = curl;
        transfer.maxSize = maxSize;
        transfer.progress = progress;
        transfer.stop = std::move(stop);

        char errorText[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorText);
        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        // Signals are not curl's to use in a program with threads; DNS timeouts then rely on the threaded resolver.
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        // Redirects are followed, but never from HTTPS down to HTTP.
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!settings_.caFile.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, settings_.caFile.c_str());
        if (!settings_.caPath.empty()) curl_easy_setopt(curl, CURLOPT_CAPATH, settings_.caPath.c_str());
        if (!settings_.noProxy.empty()) curl_easy_setopt(curl, CURLOPT_NOPROXY, settings_.noProxy.c_str());
        // The timeouts of the Windows client: 10 s to connect, then 30 s without a byte ends it.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, settings_.lowSpeedSeconds);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);

        const bool hasBody = !request.body.empty();
        if (request.method == "GET") {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else if (request.method == "HEAD") {
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        } else {
            if (request.method == "POST") {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
            } else {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
            }
            if (hasBody || request.method == "POST") {
                // The body lives in `request` until perform returns; curl does not copy it.
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            }
        }

        curl_slist* rawHeaders = nullptr;
        bool namesType = false;
        for (const auto& [name, value] : request.headers) {
            rawHeaders = curl_slist_append(rawHeaders, (name + ": " + value).c_str());
            std::string lower = name;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "content-type") namesType = true;
        }
        // WinHTTP sends no type of its own, curl would claim a form; and no "Expect: 100-continue" wait.
        if (!namesType && request.method != "GET" && request.method != "HEAD") {
            rawHeaders = curl_slist_append(rawHeaders, "Content-Type:");
        }
        rawHeaders = curl_slist_append(rawHeaders, "Expect:");
        const std::unique_ptr<curl_slist, ListCloser> headers(rawHeaders);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());

        const CURLcode result = curl_easy_perform(curl);

        switch (transfer.abort) {
        case Transfer::Abort::Rejected: return rejected(transfer.status);
        case Transfer::Abort::TooLarge: return Error{ErrorCode::InvalidData, "the download is larger than expected"};
        case Transfer::Abort::Canceled: return Error{ErrorCode::Canceled, "the download was cancelled"};
        case Transfer::Abort::None: break;
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (result == CURLE_UNSUPPORTED_PROTOCOL && status >= 300 && status < 400) {
            // A redirect to plain HTTP: WinHTTP stops there and hands back the redirect itself.
            return rejected(status);
        }
        if (result != CURLE_OK) {
            std::string message = std::string("curl: ") + curl_easy_strerror(result);
            if (errorText[0]) message += std::string(" (") + errorText + ")";
            return Error{ErrorCode::Network, message};
        }
        if (status != 200) return rejected(status); // an answer without a body never reached onBody
        return std::move(transfer.body);
    }

    std::string userAgent_;
    CurlSettings settings_;
};

} // namespace

std::string caBundle() {
    if (const char* chosen = std::getenv("SSL_CERT_FILE"); isFile(chosen)) return chosen;
    for (const char* bundle :
         {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
          "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem"}) {
        if (isFile(bundle)) return bundle;
    }
    return {};
}

std::string caDirectory() {
    if (const char* chosen = std::getenv("SSL_CERT_DIR"); isFolder(chosen)) return chosen;
    for (const char* folder : {"/etc/ssl/certs", "/etc/pki/tls/certs"}) {
        if (isFolder(folder)) return folder;
    }
    return {};
}

std::unique_ptr<update::IHttpClient> createCurlHttpClient(std::string userAgent) {
    return createCurlHttpClient(std::move(userAgent), CurlSettings{});
}

std::unique_ptr<update::IHttpClient> createCurlHttpClient(std::string userAgent, CurlSettings settings) {
    return std::make_unique<CurlHttpClient>(std::move(userAgent), std::move(settings));
}

} // namespace veloplay::linuxos
