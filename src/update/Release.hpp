#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "update/Version.hpp"
#include "util/Result.hpp"

namespace veloplay::update {

/// Where releases are published and where the help pages live. The updater
/// refuses downloads from anywhere else. This is the public repository; it
/// receives the source and the program with every release. Development
/// happens in a private one.
inline constexpr std::string_view kReleaseRepository = "FynnXland/VeloPlay";
inline constexpr std::string_view kReleaseAssetName = "VeloPlay-windows-x64.zip";

std::string latestReleaseApiUrl();
std::string releasesPageUrl();

/// What the updater needs to know about a published release.
struct ReleaseInfo {
    Version version;
    std::string pageUrl;     // release notes, shown in the browser
    std::string downloadUrl; // the Windows archive
    std::string sha256;      // lowercase hex digest of the archive
    std::uint64_t size = 0;

    friend bool operator==(const ReleaseInfo&, const ReleaseInfo&) = default;
};

/// Parses the answer of GitHub's "latest release" API. Fails if the release has
/// no usable version tag, no Windows archive, no checksum, or if the download
/// address does not point into our repository.
Result<ReleaseInfo> parseLatestRelease(std::string_view json);

} // namespace veloplay::update
