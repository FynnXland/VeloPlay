#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace veloplay::update {

/// Three-part version number as used for releases ("0.2.0", tag "v0.2.0").
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    /// Accepts "1.2.3" and "v1.2.3". Anything else, including pre-release
    /// suffixes, is rejected: the updater only offers finished releases.
    static std::optional<Version> parse(std::string_view text);

    [[nodiscard]] std::string toString() const;

    friend auto operator<=>(const Version&, const Version&) = default;
};

} // namespace veloplay::update
