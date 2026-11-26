#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "util/Result.hpp"

namespace veloplay::setup {

/// VeloPlay-Setup.exe is one file to double-click: a small program with the
/// package of a release (the same zip the updater fetches) attached behind it.
///
/// The package sits right behind the last section of the program, announced by
/// this mark and its length (eight bytes, little endian). It is found from the
/// program's own headers and not from the end of the file: a signature, should
/// the setup ever get one, is appended at the very end.
inline constexpr char kPayloadMark[8] = {'V', 'P', 'S', 'E', 'T', 'U', 'P', '1'};
inline constexpr std::uint64_t kMaxPayloadBytes = 512ull * 1024 * 1024;

/// Where a Windows program ends as far as its own headers say: the end of the
/// section that reaches furthest into the file. `headers` is the start of the
/// file; the first 64 KiB hold more than any program's headers.
Result<std::uint64_t> programEnd(std::span<const std::uint8_t> headers);

/// The package attached to `setupFile`.
Result<std::vector<std::uint8_t>> readPayload(const std::filesystem::path& setupFile);

/// Writes `program` with `package` attached to `setupFile`. Refuses a program
/// that already carries something behind its sections: the package would not
/// be found there.
Result<void> writeSetup(const std::filesystem::path& program, std::span<const std::uint8_t> package,
                        const std::filesystem::path& setupFile);

} // namespace veloplay::setup
