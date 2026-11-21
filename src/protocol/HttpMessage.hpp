#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstdint>

// The console speaks an HTTP-like text format in three places: discovery
// datagrams, the registration exchange and the session request. None of them is
// real HTTP (discovery uses bare LF and "key:value" without a space), so this
// parser is lenient about line endings and spacing.

namespace veloplay::protocol {

class HttpHeaders {
public:
    void add(std::string name, std::string value);

    /// Case-insensitive lookup. Returns the first match.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const;

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const { return entries_; }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

struct HttpResponse {
    int status = 0;
    std::string reason;
    HttpHeaders headers;
};

/// Parses "name: value" lines until the first empty line or the end of input.
/// Lines without a colon are skipped. NUL bytes end the input.
HttpHeaders parseHeaderLines(std::string_view text);

/// Parses a status line ("HTTP/1.1 200 Ok") followed by header lines.
std::optional<HttpResponse> parseHttpResponse(std::string_view text);

/// Offset just past the blank line that ends an HTTP header block
/// ("\r\n\r\n"), or nullopt if the block is not complete yet.
std::optional<std::size_t> findHeaderEnd(std::span<const std::uint8_t> data);

} // namespace veloplay::protocol
