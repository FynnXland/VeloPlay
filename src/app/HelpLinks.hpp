#pragma once

#include <string>
#include <string_view>

namespace veloplay {

/// Start of every link into the repository. "HEAD" is whatever branch GitHub
/// shows by default, so the links keep working when that branch is renamed.
std::string helpBaseUrl();

/// Web address the client opens for a help topic the screens ask for:
/// "account-id", "remote-control", "releases", "release-notes". Anything else
/// leads to the readme.
std::string helpUrl(std::string_view topic, bool german);

} // namespace veloplay
