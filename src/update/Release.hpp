#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "update/Package.hpp"
#include "update/Version.hpp"
#include "util/Result.hpp"

namespace veloplay::update {

/// Where releases are published and where the help pages live. The updater
/// refuses downloads from anywhere else. This is the public repository; it
/// receives the source and the program with every release. Development
/// happens in a private one.
inline constexpr std::string_view kReleaseRepository = "FynnXland/VeloPlay";
/// The file of the release for this program's platform (see update/Package.hpp).
inline constexpr std::string_view kReleaseAssetName = kThisPackage.releaseAsset;

std::string latestReleaseApiUrl();
std::string releasesPageUrl();

/// What the updater needs to know about a published release.
struct ReleaseInfo {
    Version version;
    std::string pageUrl;     // release notes, shown in the browser
    std::string downloadUrl; // the archive of the platform asked for
    std::string sha256;      // lowercase hex digest of the archive
    std::uint64_t size = 0;

    friend bool operator==(const ReleaseInfo&, const ReleaseInfo&) = default;
};

/// Parses the answer of GitHub's "latest release" API for the archive of
/// `package`. Fails if the release has no usable version tag, no archive for
/// `package`, no checksum, or if the download address does not point into our
/// repository.
Result<ReleaseInfo> parseLatestRelease(std::string_view json, const PackageLayout& package = kThisPackage);

/// The same, but a release that is fine and only lists no archive for
/// `package` gives nullopt instead of failing. The Linux file is published a
/// few minutes after the release; until then the release is not an update yet.
Result<std::optional<ReleaseInfo>> findRelease(std::string_view json, const PackageLayout& package = kThisPackage);

} // namespace veloplay::update
