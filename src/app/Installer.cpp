#include "app/Installer.hpp"

#include <cctype>
#include <string>
#include <system_error>

namespace veloplay::install {

bool belongsToPackage(const std::filesystem::path& file) {
    const std::filesystem::path name = file.filename();
    std::string extension = utf8(name.extension());
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".exe" || extension == ".dll") return true;
    for (const char* document : {"README.md", "README.de.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY.md"}) {
        if (name == document) return true;
    }
    return false;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

Result<int> copyProgram(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (!std::filesystem::is_directory(from, error)) {
        return Error{ErrorCode::InvalidArgument, "the folder to install from does not exist: " + utf8(from)};
    }
    if (isInside(to, from)) {
        return Error{ErrorCode::InvalidArgument, "the folder to install into lies inside the one to install from"};
    }
    std::filesystem::create_directories(to, error);
    if (error) return Error{ErrorCode::Io, "could not create " + utf8(to) + ": " + error.message()};

    if (!std::filesystem::is_regular_file(from / kProgramFile, error)) {
        return Error{ErrorCode::InvalidData, std::string("there is no ") + kProgramFile + " in " + utf8(from)};
    }
    int copied = 0;
    for (auto entry = std::filesystem::directory_iterator(from, error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        // Only files directly in the folder: the package has no folders. What an update moved out of the
        // way (*.old) and whatever else lies there is not VeloPlay's.
        if (!entry->is_regular_file(error) || !belongsToPackage(entry->path())) continue;
        const std::filesystem::path destination = to / entry->path().filename();
        std::filesystem::copy_file(entry->path(), destination, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return Error{ErrorCode::Io, "could not copy " + utf8(entry->path().filename()) + ": " + error.message()};
        }
        ++copied;
    }
    if (error) return Error{ErrorCode::Io, "could not read " + utf8(from) + ": " + error.message()};
    return copied;
}

bool isInside(const std::filesystem::path& file, const std::filesystem::path& folder) {
    std::error_code error;
    const std::filesystem::path inner = std::filesystem::weakly_canonical(file, error);
    if (error) return false;
    const std::filesystem::path outer = std::filesystem::weakly_canonical(folder, error);
    if (error || outer.empty()) return false;
    auto innerPart = inner.begin();
    for (auto outerPart = outer.begin(); outerPart != outer.end(); ++outerPart, ++innerPart) {
        if (outerPart->empty()) continue; // a trailing separator
        if (innerPart == inner.end() || *innerPart != *outerPart) return false;
    }
    return true;
}

bool isRemovable(const std::filesystem::path& folder) {
    std::error_code error;
    if (folder.filename() != "VeloPlay") return false;
    if (!folder.has_parent_path() || folder.parent_path() == folder.root_path()) return false;
    return std::filesystem::is_regular_file(folder / kProgramFile, error);
}

} // namespace veloplay::install
