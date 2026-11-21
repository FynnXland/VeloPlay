#include "update/Zip.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>

#include <miniz.h>

namespace veloplay::update {

namespace {

struct ArchiveCloser {
    void operator()(mz_zip_archive* archive) const { mz_zip_reader_end(archive); }
};

struct HeapFree {
    void operator()(void* memory) const { mz_free(memory); }
};

} // namespace

bool isSafeArchivePath(std::string_view name) {
    if (name.empty() || name.size() > 240) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    if (name.find(':') != std::string_view::npos) return false; // drive letters, alternate data streams
    if (name.find('\0') != std::string_view::npos) return false;

    // Check every path component, with either kind of separator.
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = std::min(name.find_first_of("/\\", start), name.size());
        const std::string_view part = name.substr(start, end - start);
        if (part == "..") return false;
        start = end + 1;
    }
    return true;
}

Result<std::vector<std::filesystem::path>> extractZip(std::span<const std::uint8_t> archive,
                                                      const std::filesystem::path& destination) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) {
        return Error{ErrorCode::InvalidData, "the download is not a valid zip archive"};
    }
    const std::unique_ptr<mz_zip_archive, ArchiveCloser> closer(&zip);

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count == 0 || count > kMaxArchiveFiles) {
        return Error{ErrorCode::InvalidData, "the archive has an unexpected number of files"};
    }

    // First pass: validate everything before a single file is written.
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            return Error{ErrorCode::InvalidData, "the archive directory is damaged"};
        }
        if (!isSafeArchivePath(stat.m_filename)) {
            return Error{ErrorCode::InvalidData, std::string("the archive contains an unsafe path: ") + stat.m_filename};
        }
        total += stat.m_uncomp_size;
        if (total > kMaxExtractedBytes) return Error{ErrorCode::InvalidData, "the archive unpacks to too much data"};
    }

    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) return Error{ErrorCode::Io, "cannot create " + destination.string()};

    std::vector<std::filesystem::path> written;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        mz_zip_reader_file_stat(&zip, i, &stat);
        // Zip names are UTF-8.
        const std::string_view name = stat.m_filename;
        const std::filesystem::path relative = std::filesystem::path(std::u8string(name.begin(), name.end())).lexically_normal();
        const std::filesystem::path target = destination / relative;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::filesystem::create_directories(target, ec);
            continue;
        }

        std::size_t size = 0;
        const std::unique_ptr<void, HeapFree> data(mz_zip_reader_extract_to_heap(&zip, i, &size, 0));
        if (!data && stat.m_uncomp_size != 0) {
            return Error{ErrorCode::InvalidData, std::string("cannot unpack ") + stat.m_filename};
        }

        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (size > 0) file.write(static_cast<const char*>(data.get()), static_cast<std::streamsize>(size));
        file.flush();
        if (!file) return Error{ErrorCode::Io, "cannot write " + target.string()};
        written.push_back(relative);
    }
    return written;
}

} // namespace veloplay::update
