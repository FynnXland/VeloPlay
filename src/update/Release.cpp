#include "update/Release.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace veloplay::update {

namespace {

using json = nlohmann::json;

std::string text(const json& object, const char* key) {
    const auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

bool isLowerHex(std::string_view value) {
    return std::all_of(value.begin(), value.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

std::string latestReleaseApiUrl() {
    return "https://api.github.com/repos/" + std::string(kReleaseRepository) + "/releases/latest";
}

std::string releasesPageUrl() {
    return "https://github.com/" + std::string(kReleaseRepository) + "/releases/latest";
}

Result<ReleaseInfo> parseLatestRelease(std::string_view jsonText) {
    const json root = json::parse(jsonText, nullptr, false);
    if (!root.is_object()) return Error{ErrorCode::InvalidData, "the release information is not valid JSON"};

    const auto version = Version::parse(text(root, "tag_name"));
    if (!version) return Error{ErrorCode::InvalidData, "the latest release has no version tag like v1.2.3"};

    ReleaseInfo info;
    info.version = *version;
    info.pageUrl = text(root, "html_url");

    const std::string prefix = "https://github.com/" + std::string(kReleaseRepository) + "/releases/";
    if (!info.pageUrl.starts_with(prefix)) info.pageUrl = releasesPageUrl();

    const auto assets = root.find("assets");
    if (assets == root.end() || !assets->is_array()) {
        return Error{ErrorCode::InvalidData, "the latest release has no files"};
    }
    for (const json& asset : *assets) {
        if (!asset.is_object() || text(asset, "name") != kReleaseAssetName) continue;

        info.downloadUrl = text(asset, "browser_download_url");
        // Never follow a download address that leaves our repository, whatever the API says.
        if (!info.downloadUrl.starts_with(prefix + "download/")) {
            return Error{ErrorCode::InvalidData, "the download address of the release is not on our release page"};
        }

        // GitHub publishes the digest of every uploaded file as "sha256:<hex>".
        const std::string digest = text(asset, "digest");
        constexpr std::string_view kScheme = "sha256:";
        if (!digest.starts_with(kScheme) || digest.size() != kScheme.size() + 64 ||
            !isLowerHex(std::string_view(digest).substr(kScheme.size()))) {
            return Error{ErrorCode::InvalidData, "the release has no SHA-256 checksum"};
        }
        info.sha256 = digest.substr(kScheme.size());

        const auto size = asset.find("size");
        if (size != asset.end() && size->is_number_unsigned()) info.size = size->get<std::uint64_t>();
        return info;
    }
    return Error{ErrorCode::InvalidData, "the latest release has no Windows download"};
}

} // namespace veloplay::update
