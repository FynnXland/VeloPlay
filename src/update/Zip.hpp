#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "util/Bytes.hpp"
#include "util/Result.hpp"

namespace veloplay::update {

inline constexpr std::size_t kMaxArchiveFiles = 2000;
inline constexpr std::uint64_t kMaxExtractedBytes = 1024ull * 1024 * 1024;

/// Unpacks a zip archive held in memory into `destination` and returns the
/// relative paths of the files it wrote.
///
/// Entries that would land outside `destination` (absolute paths, "..", drive
/// letters) make the whole extraction fail, as do archives that unpack to an
/// unreasonable size.
Result<std::vector<std::filesystem::path>> extractZip(std::span<const std::uint8_t> archive,
                                                      const std::filesystem::path& destination);

/// True for names that are safe to create below an extraction folder.
bool isSafeArchivePath(std::string_view name);

} // namespace veloplay::update
