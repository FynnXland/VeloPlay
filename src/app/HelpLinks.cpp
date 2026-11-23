#include "app/HelpLinks.hpp"

#include "update/Release.hpp"

namespace veloplay {

std::string helpBaseUrl() { return "https://github.com/" + std::string(update::kReleaseRepository) + "/blob/HEAD/"; }

std::string helpUrl(std::string_view topic, bool german) {
    if (topic == "releases" || topic == "release-notes") return update::releasesPageUrl();
    const std::string readme = german ? "README.de.md" : "README.md";
    if (topic == "account-id") return helpBaseUrl() + readme + "#account-id";
    if (topic == "away") return helpBaseUrl() + readme + "#away";
    if (topic == "remote-control") return helpBaseUrl() + "docs/remote_control.md";
    return helpBaseUrl() + readme;
}

} // namespace veloplay
