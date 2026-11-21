#include "update/Version.hpp"

#include <charconv>

namespace veloplay::update {

std::optional<Version> Version::parse(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);

    Version version;
    int* parts[] = {&version.major, &version.minor, &version.patch};
    const char* p = text.data();
    const char* end = p + text.size();
    for (int i = 0; i < 3; ++i) {
        const auto [next, ec] = std::from_chars(p, end, *parts[i]);
        if (ec != std::errc{} || next == p || *parts[i] < 0 || *parts[i] > 9999) return std::nullopt;
        p = next;
        if (i < 2) {
            if (p == end || *p != '.') return std::nullopt;
            ++p;
        }
    }
    if (p != end) return std::nullopt;
    return version;
}

std::string Version::toString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

} // namespace veloplay::update
