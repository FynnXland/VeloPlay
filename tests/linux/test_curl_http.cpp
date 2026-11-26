// The curl client against an HTTPS server in this process: a certificate made
// for 127.0.0.1 on the spot, trusted through CurlSettings::caFile only.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "linux/LinuxServices.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

/// A key and a self-signed certificate for 127.0.0.1, the certificate also as a PEM file.
struct TestCertificate {
    EVP_PKEY* key = nullptr;
    X509* certificate = nullptr;
    std::filesystem::path pemFile;

    TestCertificate() {
        key = EVP_EC_gen("P-256");
        certificate = X509_new();
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
        X509_gmtime_adj(X509_getm_notBefore(certificate), -60);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
        X509_set_pubkey(certificate, key);
        X509_NAME* name = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("127.0.0.1"), -1,
                                   -1, 0);
        X509_set_issuer_name(certificate, name);
        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
        for (const auto& [nid, value] :
             {std::pair{NID_subject_alt_name, "IP:127.0.0.1"}, std::pair{NID_basic_constraints, "critical,CA:TRUE"}}) {
            X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
            X509_add_ext(certificate, extension, -1);
            X509_EXTENSION_free(extension);
        }
        X509_sign(certificate, key, EVP_sha256());

        // One file per process: ctest runs the tests side by side, each with a certificate of its own.
        static int made = 0;
        pemFile = std::filesystem::temp_directory_path() / "veloplay_tests" /
                  ("linux_http_ca_" + std::to_string(::getpid()) + "_" + std::to_string(++made) + ".pem");
        std::filesystem::create_directories(pemFile.parent_path());
        BIO* out = BIO_new_file(pemFile.c_str(), "w");
        PEM_write_bio_X509(out, certificate);
        BIO_free(out);
    }
    ~TestCertificate() {
        std::error_code ignored;
        std::filesystem::remove(pemFile, ignored);
        X509_free(certificate);
        EVP_PKEY_free(key);
    }
};

struct Request {
    std::string head; // request line and headers
    std::string body;
};

/// What the server does with one request: writes an answer through `send`.
/// `quit` turns true when the test is over.
using Handler = std::function<void(const Request&, const std::function<bool(std::string_view)>& send,
                                   const std::atomic<bool>& quit)>;

class TlsServer {
public:
    TlsServer(const TestCertificate& identity, Handler handler) : handler_(std::move(handler)) {
        // Writing to a client that went away (the test of cancelling) must fail, not end the test program.
        std::signal(SIGPIPE, SIG_IGN);
        context_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(context_, identity.certificate);
        SSL_CTX_use_PrivateKey(context_, identity.key);
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        socklen_t size = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        ::listen(listener_, 8);
        thread_ = std::thread([this] { serve(); });
    }

    ~TlsServer() {
        quit_ = true;
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        thread_.join();
        SSL_CTX_free(context_);
    }

    [[nodiscard]] std::string url(const std::string& path) const {
        return "https://127.0.0.1:" + std::to_string(port_) + path;
    }

    std::vector<Request> requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void serve() {
        while (!quit_) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) return;
            timeval timeout{5, 0}; // a test that goes wrong must not hang here
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            SSL* ssl = SSL_new(context_);
            SSL_set_fd(ssl, client);
            if (SSL_accept(ssl) == 1) answer(ssl);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(client);
        }
    }

    void answer(SSL* ssl) {
        std::string received;
        char chunk[4096];
        std::size_t end = std::string::npos;
        while ((end = received.find("\r\n\r\n")) == std::string::npos) {
            const int got = SSL_read(ssl, chunk, sizeof(chunk));
            if (got <= 0) return;
            received.append(chunk, static_cast<std::size_t>(got));
        }
        Request request{received.substr(0, end), received.substr(end + 4)};
        std::string lower = request.head;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (const auto at = lower.find("content-length:"); at != std::string::npos) {
            const std::size_t length = std::stoul(request.head.substr(at + 15));
            while (request.body.size() < length) {
                const int got = SSL_read(ssl, chunk, sizeof(chunk));
                if (got <= 0) return;
                request.body.append(chunk, static_cast<std::size_t>(got));
            }
        }
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        handler_(
            request,
            [ssl](std::string_view bytes) {
                return bytes.empty() || SSL_write(ssl, bytes.data(), static_cast<int>(bytes.size())) > 0;
            },
            quit_);
    }

    Handler handler_;
    SSL_CTX* context_ = nullptr;
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> quit_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Request> requests_;
};

std::string reply(int status, const std::string& body, const std::string& extra = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

std::string text(const Bytes& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

/// The client as the program has it, but trusting only the test's certificate and never proxied.
std::unique_ptr<update::IHttpClient> testClient(const TestCertificate& identity) {
    linuxos::CurlSettings settings;
    settings.caFile = identity.pemFile.string();
    settings.caPath.clear();
    settings.noProxy = "*";
    return linuxos::createCurlHttpClient("VeloPlay/test", settings);
}

} // namespace

TEST(LinuxHttp, FetchesOverHttpsAndReportsProgress) {
    const TestCertificate identity;
    TlsServer server(identity, [](const Request& request, const auto& send, const auto&) {
        if (request.head.starts_with("GET /release "))
            send(reply(200, std::string(100'000, 'x')));
        else
            send(reply(404, "not here"));
    });
    const auto http = testClient(identity);
    std::uint64_t lastReceived = 0;
    std::uint64_t lastTotal = 0;
    const auto answer = http->get(server.url("/release"), 1'000'000,
                                  [&](std::uint64_t received, std::uint64_t total) {
                                      EXPECT_GE(received, lastReceived);
                                      lastReceived = received;
                                      lastTotal = total;
                                  },
                                  {});
    ASSERT_TRUE(answer) << answer.error().message;
    EXPECT_EQ(answer->size(), 100'000u);
    EXPECT_EQ(lastReceived, 100'000u);
    EXPECT_EQ(lastTotal, 100'000u);
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_NE(requests[0].head.find("User-Agent: VeloPlay/test"), std::string::npos);
    EXPECT_NE(requests[0].head.find("Accept: application/vnd.github+json"), std::string::npos);
}

TEST(LinuxHttp, AcceptsOnlyStatus200AndSaysWhichCameInstead) {
    const TestCertificate identity;
    TlsServer server(identity, [](const Request& request, const auto& send, const auto&) {
        // A 404 with a page larger than allowed is still a 404, as on Windows.
        if (request.head.starts_with("GET /big404 "))
            send(reply(404, std::string(5000, 'e')));
        else if (request.head.starts_with("GET /empty "))
            send(reply(204, ""));
        else
            send(reply(404, "not here"));
    });
    const auto http = testClient(identity);
    const auto missing = http->get(server.url("/nothing"), 1000, {}, {});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::Rejected);
    EXPECT_EQ(missing.error().message, "the server answered with HTTP 404");
    const auto big = http->get(server.url("/big404"), 100, {}, {});
    ASSERT_FALSE(big);
    EXPECT_EQ(big.error().code, ErrorCode::Rejected);
    const auto empty = http->get(server.url("/empty"), 100, {}, {});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().message, "the server answered with HTTP 204");
}

TEST(LinuxHttp, RefusesWhatIsNotHttpsAndRedirectsDownToHttp) {
    const TestCertificate identity;
    std::string self;
    TlsServer server(identity, [&self](const Request& request, const auto& send, const auto&) {
        if (request.head.starts_with("GET /down ")) {
            send(reply(302, "", "Location: http://127.0.0.1:1/plain\r\n"));
        } else if (request.head.starts_with("GET /up ")) {
            send(reply(302, "", "Location: " + self + "/final\r\n"));
        } else if (request.head.starts_with("GET /final ")) {
            send(reply(200, "arrived"));
        } else {
            send(reply(404, ""));
        }
    });
    self = server.url("");
    const auto http = testClient(identity);
    for (const char* url : {"http://127.0.0.1/x", "ftp://127.0.0.1/x", "file:///etc/passwd", "127.0.0.1/x", ""}) {
        const auto plain = http->get(url, 100, {}, {});
        ASSERT_FALSE(plain) << url;
        EXPECT_EQ(plain.error().code, ErrorCode::InvalidArgument) << url;
    }
    const auto down = http->get(server.url("/down"), 100, {}, {});
    ASSERT_FALSE(down);
    EXPECT_EQ(down.error().code, ErrorCode::Rejected) << down.error().message;
    const auto up = http->get(server.url("/up"), 100, {}, {});
    ASSERT_TRUE(up) << up.error().message;
    EXPECT_EQ(text(*up), "arrived");
}

TEST(LinuxHttp, StopsAtTheSizeAskedFor) {
    const TestCertificate identity;
    TlsServer server(identity, [](const Request& request, const auto& send, const auto&) {
        if (request.head.starts_with("GET /announced ")) {
            send(reply(200, std::string(5000, 'a')));
        } else {
            // No length beforehand: only counting the bytes finds out.
            send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
            for (int i = 0; i < 10; ++i)
                send("400\r\n" + std::string(0x400, 'c') + "\r\n");
            send("0\r\n\r\n");
        }
    });
    const auto http = testClient(identity);
    for (const char* path : {"/announced", "/chunked"}) {
        const auto answer = http->get(server.url(path), 4096, {}, {});
        ASSERT_FALSE(answer) << path;
        EXPECT_EQ(answer.error().code, ErrorCode::InvalidData) << path;
    }
    const auto fits = http->get(server.url("/chunked"), 20'000, {}, {});
    ASSERT_TRUE(fits) << fits.error().message;
    EXPECT_EQ(fits->size(), 10u * 0x400);
}

TEST(LinuxHttp, EndsAWaitingDownloadWhenAskedToStop) {
    const TestCertificate identity;
    TlsServer server(identity, [](const Request&, const auto& send, const std::atomic<bool>& quit) {
        // Says much is coming, then trickles: the client must not wait for it all.
        send("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\nConnection: close\r\n\r\n");
        for (int i = 0; i < 200 && !quit; ++i) {
            if (!send("x")) return;
            std::this_thread::sleep_for(20ms);
        }
    });
    const auto http = testClient(identity);
    std::stop_source stop;
    std::thread stopper([&] {
        std::this_thread::sleep_for(300ms);
        stop.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto answer = http->get(server.url("/slow"), 2'000'000, {}, stop.get_token());
    stopper.join();
    ASSERT_FALSE(answer);
    EXPECT_EQ(answer.error().code, ErrorCode::Canceled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);

    std::stop_source already;
    already.request_stop();
    const auto never = http->get(server.url("/slow"), 100, {}, already.get_token());
    ASSERT_FALSE(never);
    EXPECT_EQ(never.error().code, ErrorCode::Canceled);
}

// PsnAccountLookup sends forms with POST; the body and its type arrive as given.
TEST(LinuxHttp, SendsARequestWithMethodHeadersAndBody) {
    const TestCertificate identity;
    TlsServer server(identity, [](const Request& request, const auto& send, const auto&) {
        send(reply(200, request.head.starts_with("POST /token ") ? "{\"ok\":true}" : "wrong"));
    });
    const auto http = testClient(identity);
    update::HttpRequest request;
    request.method = "POST";
    request.url = server.url("/token");
    request.headers = {{"Authorization", "Basic abc"}, {"Content-Type", "application/x-www-form-urlencoded"}};
    request.body = "grant_type=authorization_code&code=123";
    const auto answer = http->send(request, 1000, {});
    ASSERT_TRUE(answer) << answer.error().message;
    EXPECT_EQ(text(*answer), "{\"ok\":true}");
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].body, request.body);
    EXPECT_NE(requests[0].head.find("Authorization: Basic abc"), std::string::npos);
    EXPECT_NE(requests[0].head.find("Content-Type: application/x-www-form-urlencoded"), std::string::npos);
    EXPECT_EQ(requests[0].head.find("Expect:"), std::string::npos);
}

// Certificates are checked: a server the bundle does not vouch for is a network error, never accepted.
TEST(LinuxHttp, RefusesAServerItCannotVerify) {
    const TestCertificate identity;
    TlsServer server(identity, [](const Request&, const auto& send, const auto&) { send(reply(200, "secret")); });
    linuxos::CurlSettings settings;
    settings.caFile = linuxos::caBundle(); // the system's authorities, which never signed this one
    settings.noProxy = "*";
    const auto http = linuxos::createCurlHttpClient("VeloPlay/test", settings);
    const auto answer = http->get(server.url("/"), 100, {}, {});
    ASSERT_FALSE(answer);
    EXPECT_EQ(answer.error().code, ErrorCode::Network) << answer.error().message;
}

TEST(LinuxHttp, FindsTheSystemsCertificateAuthorities) {
    // Every distribution the program is built for has one of the bundles; this machine too.
    const std::string bundle = linuxos::caBundle();
    ASSERT_FALSE(bundle.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(bundle));
}
