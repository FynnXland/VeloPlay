#pragma once

#include <filesystem>
#include <string_view>

namespace veloplay {

/// Makes room for a new log without losing the old ones: `<name>.log` becomes
/// `<name>.1.log`, that one `<name>.2.log` and so on; the one numbered `keep`
/// is deleted. The log of a run that hung or crashed is what a bug report
/// needs, and the program is usually started again before anyone looks.
///
/// Returns the path of the new log. A file that cannot be moved (still open
/// in another program) stays where it is; the caller then writes over it.
std::filesystem::path rotateLogFiles(const std::filesystem::path& directory, std::string_view name, int keep);

} // namespace veloplay
