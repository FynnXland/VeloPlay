#include "setup/Payload.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace veloplay::setup {

namespace {

constexpr std::size_t kHeaderBytes = 64 * 1024;

std::uint32_t read32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

Error notAProgram() { return Error{ErrorCode::InvalidData, "the file is not a Windows program"}; }

std::vector<std::uint8_t> readStart(std::ifstream& file, std::size_t limit) {
    std::vector<std::uint8_t> bytes(limit);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)));
    file.clear();
    return bytes;
}

} // namespace

Result<std::uint64_t> programEnd(std::span<const std::uint8_t> headers) {
    // "MZ", then at 0x3C the place of "PE\0\0" and the file header behind it.
    if (headers.size() < 0x40 || headers[0] != 'M' || headers[1] != 'Z') return notAProgram();
    const std::size_t pe = read32(headers, 0x3C);
    if (pe > headers.size() || headers.size() - pe < 24) return notAProgram();
    if (headers[pe] != 'P' || headers[pe + 1] != 'E' || headers[pe + 2] != 0 || headers[pe + 3] != 0) return notAProgram();
    const std::size_t sections = read16(headers, pe + 6);
    const std::size_t optionalHeader = read16(headers, pe + 20);
    const std::size_t table = pe + 24 + optionalHeader;
    constexpr std::size_t kSectionEntry = 40;
    if (sections == 0 || table > headers.size() || (headers.size() - table) / kSectionEntry < sections) return notAProgram();

    std::uint64_t end = 0;
    for (std::size_t i = 0; i < sections; ++i) {
        const std::size_t entry = table + i * kSectionEntry;
        const std::uint64_t size = read32(headers, entry + 16);
        const std::uint64_t start = read32(headers, entry + 20);
        if (size != 0) end = std::max(end, start + size);
    }
    if (end == 0) return notAProgram();
    return end;
}

Result<std::vector<std::uint8_t>> readPayload(const std::filesystem::path& setupFile) {
    std::ifstream file(setupFile, std::ios::binary);
    if (!file) return Error{ErrorCode::Io, "the setup file cannot be read"};
    const std::vector<std::uint8_t> headers = readStart(file, kHeaderBytes);
    const auto end = programEnd(headers);
    if (!end) return end.error();

    file.seekg(0, std::ios::end);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(file.tellg());
    constexpr std::uint64_t kAnnouncement = sizeof(kPayloadMark) + 8;
    if (*end > fileSize || fileSize - *end < kAnnouncement) {
        return Error{ErrorCode::InvalidData, "the setup file carries no package"};
    }
    std::uint8_t announcement[kAnnouncement];
    file.seekg(static_cast<std::streamoff>(*end));
    file.read(reinterpret_cast<char*>(announcement), sizeof(announcement));
    if (!file || std::memcmp(announcement, kPayloadMark, sizeof(kPayloadMark)) != 0) {
        return Error{ErrorCode::InvalidData, "the setup file carries no package"};
    }
    std::uint64_t length = 0;
    for (int i = 7; i >= 0; --i) length = length << 8 | announcement[sizeof(kPayloadMark) + i];
    if (length == 0 || length > kMaxPayloadBytes || length > fileSize - *end - kAnnouncement) {
        return Error{ErrorCode::InvalidData, "the package in the setup file is cut off"};
    }
    std::vector<std::uint8_t> package(static_cast<std::size_t>(length));
    file.read(reinterpret_cast<char*>(package.data()), static_cast<std::streamsize>(package.size()));
    if (!file) return Error{ErrorCode::Io, "the package in the setup file cannot be read"};
    return package;
}

Result<void> writeSetup(const std::filesystem::path& program, std::span<const std::uint8_t> package,
                        const std::filesystem::path& setupFile) {
    if (package.empty() || package.size() > kMaxPayloadBytes) {
        return Error{ErrorCode::InvalidArgument, "the package is empty or too large"};
    }
    std::ifstream in(program, std::ios::binary);
    if (!in) return Error{ErrorCode::Io, "the setup program cannot be read"};
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto end = programEnd(std::span(bytes).first(std::min(bytes.size(), kHeaderBytes)));
    if (!end) return end.error();
    if (*end != bytes.size()) {
        // Symbols of a build that was not stripped, or a package attached before.
        return Error{ErrorCode::InvalidData, "the setup program already carries data behind its sections"};
    }

    std::ofstream out(setupFile, std::ios::binary | std::ios::trunc);
    if (!out) return Error{ErrorCode::Io, "the setup file cannot be written"};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.write(kPayloadMark, sizeof(kPayloadMark));
    char length[8];
    for (int i = 0; i < 8; ++i) length[i] = static_cast<char>(static_cast<std::uint64_t>(package.size()) >> (8 * i) & 0xFF);
    out.write(length, sizeof(length));
    out.write(reinterpret_cast<const char*>(package.data()), static_cast<std::streamsize>(package.size()));
    out.flush();
    if (!out) return Error{ErrorCode::Io, "the setup file cannot be written"};
    return {};
}

} // namespace veloplay::setup
