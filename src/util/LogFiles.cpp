#include "util/LogFiles.hpp"

#include <string>
#include <system_error>

namespace veloplay {

std::filesystem::path rotateLogFiles(const std::filesystem::path& directory, std::string_view name, int keep) {
    const auto numbered = [&](int index) {
        std::string file(name);
        if (index > 0) file += "." + std::to_string(index);
        return directory / (file + ".log");
    };
    std::error_code ignored;
    if (keep > 0) std::filesystem::remove(numbered(keep), ignored);
    for (int index = keep - 1; index >= 0; --index) {
        if (!std::filesystem::exists(numbered(index), ignored)) continue;
        std::filesystem::rename(numbered(index), numbered(index + 1), ignored);
    }
    return numbered(0);
}

} // namespace veloplay
