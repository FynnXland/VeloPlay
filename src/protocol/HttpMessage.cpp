#include "protocol/HttpMessage.hpp"

#include <charconv>

#include "util/Bytes.hpp"

namespace veloplay::protocol {

namespace {

/// Cuts the next line off the front of `text`. Handles LF and CRLF.
std::string_view takeLine(std::string_view& text) {
    const auto newline = text.find('\n');
    std::string_view line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

std::string_view cutAtNul(std::string_view text) {
    return text.substr(0, text.find('\0'));
}

} // namespace

void HttpHeaders::add(std::string name, std::string value) {
    entries_.emplace_back(std::move(name), std::move(value));
}

std::optional<std::string_view> HttpHeaders::get(std::string_view name) const {
    for (const auto& [key, value] : entries_) {
        if (equalsIgnoreCase(key, name)) return std::string_view{value};
    }
    return std::nullopt;
}

HttpHeaders parseHeaderLines(std::string_view text) {
    text = cutAtNul(text);
    HttpHeaders headers;
    while (!text.empty()) {
        const std::string_view line = takeLine(text);
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        headers.add(std::string(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
    }
    return headers;
}

std::optional<HttpResponse> parseHttpResponse(std::string_view text) {
    text = cutAtNul(text);
    constexpr std::string_view kPrefix = "HTTP/1.1 ";
    if (!text.starts_with(kPrefix)) return std::nullopt;

    std::string_view statusLine = takeLine(text);
    statusLine.remove_prefix(kPrefix.size());

    HttpResponse response;
    const auto [end, ec] = std::from_chars(statusLine.data(), statusLine.data() + statusLine.size(), response.status);
    if (ec != std::errc{} || response.status < 100 || response.status > 999) return std::nullopt;
    response.reason = std::string(trim(statusLine.substr(static_cast<std::size_t>(end - statusLine.data()))));
    response.headers = parseHeaderLines(text);
    return response;
}

std::optional<std::size_t> findHeaderEnd(std::span<const std::uint8_t> data) {
    constexpr std::string_view kTerminator = "\r\n\r\n";
    const auto pos = asText(data).find(kTerminator);
    if (pos == std::string_view::npos) return std::nullopt;
    return pos + kTerminator.size();
}

} // namespace veloplay::protocol
