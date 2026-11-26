#include "app/Installer.hpp"

#include <cctype>
#include <string>
#include <system_error>

namespace veloplay::install {

namespace {

/// libfoo.so, libfoo.so.1, libfoo.so.1.2.3: what the dynamic linker loads.
bool isSharedLibrary(const std::string& name) {
    const std::size_t at = name.find(".so");
    if (at == std::string::npos || at == 0) return false;
    std::size_t rest = at + 3;
    // Only version numbers may follow, each after a dot.
    while (rest < name.size()) {
        if (name[rest] != '.') return false;
        const std::size_t digits = ++rest;
        while (rest < name.size() && std::isdigit(static_cast<unsigned char>(name[rest]))) ++rest;
        if (rest == digits) return false;
    }
    return true;
}

} // namespace

bool belongsToPackage(const std::filesystem::path& file, const PackageLayout& package) {
    const std::filesystem::path name = file.filename();
    for (const char* document : {"README.md", "README.de.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY.md"}) {
        if (name == document) return true;
    }
    if (package.platform == PackageLayout::Platform::Windows) {
        std::string extension = utf8(name.extension());
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return extension == ".exe" || extension == ".dll";
    }
    // Linux names are case-sensitive and programs have no extension, so the package names its files.
    if (name == package.programFile || name == kLinuxIconFile) return true;
    for (const char* helper : update::kLinuxHelperPrograms) {
        if (name == helper) return true;
    }
    return isSharedLibrary(utf8(name));
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

Result<int> copyProgram(const std::filesystem::path& from, const std::filesystem::path& to,
                        const PackageLayout& package) {
    std::error_code error;
    if (!std::filesystem::is_directory(from, error)) {
        return Error{ErrorCode::InvalidArgument, "the folder to install from does not exist: " + utf8(from)};
    }
    if (isInside(to, from)) {
        return Error{ErrorCode::InvalidArgument, "the folder to install into lies inside the one to install from"};
    }
    std::filesystem::create_directories(to, error);
    if (error) return Error{ErrorCode::Io, "could not create " + utf8(to) + ": " + error.message()};

    if (!std::filesystem::is_regular_file(from / package.programFile, error)) {
        return Error{ErrorCode::InvalidData, std::string("there is no ") + package.programFile + " in " + utf8(from)};
    }
    int copied = 0;
    for (auto entry = std::filesystem::directory_iterator(from, error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        // Only files directly in the folder: the package has no folders. What an update moved out of the
        // way (*.old) and whatever else lies there is not VeloPlay's.
        if (!entry->is_regular_file(error) || !belongsToPackage(entry->path(), package)) continue;
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

bool isRemovable(const std::filesystem::path& folder, const PackageLayout& package) {
    std::error_code error;
    if (folder.filename() != "VeloPlay") return false;
    if (!folder.has_parent_path() || folder.parent_path() == folder.root_path()) return false;
    return std::filesystem::is_regular_file(folder / package.programFile, error);
}

} // namespace veloplay::install
