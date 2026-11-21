#include "update/Updater.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "protocol/Crypto.hpp"
#include "update/Zip.hpp"
#include "util/Bytes.hpp"

namespace veloplay::update {

namespace {

constexpr std::uint64_t kMaxApiResponse = 1024 * 1024;
constexpr std::uint64_t kMaxDownload = 300ull * 1024 * 1024;
constexpr char kOldSuffix[] = ".old";

std::filesystem::path withOldSuffix(std::filesystem::path path) {
    path += kOldSuffix;
    return path;
}

} // namespace

Result<void> swapInFiles(const std::filesystem::path& stagingDir, const std::filesystem::path& installDir,
                         const std::vector<std::filesystem::path>& files) {
    namespace fs = std::filesystem;
    std::error_code ec;

    struct Swapped {
        fs::path target;
        bool hadOld;
    };
    std::vector<Swapped> done;

    const auto rollBack = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code ignored;
            fs::remove(it->target, ignored);
            if (it->hadOld) fs::rename(withOldSuffix(it->target), it->target, ignored);
        }
    };

    for (const fs::path& relative : files) {
        const fs::path source = stagingDir / relative;
        const fs::path target = installDir / relative;
        const fs::path old = withOldSuffix(target);

        fs::create_directories(target.parent_path(), ec);
        const bool exists = fs::exists(target, ec);
        if (exists) {
            fs::remove(old, ec); // leftover of an earlier update
            fs::rename(target, old, ec);
            if (ec) {
                rollBack();
                return Error{ErrorCode::Io, "cannot move " + target.string() + " aside: " + ec.message()};
            }
        }
        done.push_back({target, exists});

        // Staging may sit on another drive than the program, so copy instead of rename.
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            rollBack();
            return Error{ErrorCode::Io, "cannot write " + target.string() + ": " + ec.message()};
        }
    }
    return {};
}

void removeLeftovers(const std::filesystem::path& installDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(installDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == kOldSuffix) {
            std::error_code ignored;
            fs::remove(it->path(), ignored);
        }
    }
}

Updater::Updater(Options options) : options_(std::move(options)) { status_.current = options_.current; }

Updater::~Updater() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

UpdateStatus Updater::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void Updater::set(UpdateStatus::Phase phase) {
    std::lock_guard lock(mutex_);
    status_.phase = phase;
    if (phase != UpdateStatus::Phase::Failed) {
        status_.error = UpdateError::None;
        status_.detail.clear();
    }
}

void Updater::fail(UpdateError error, std::string detail, bool duringInstall) {
    spdlog::warn("Update: {}", detail);
    std::lock_guard lock(mutex_);
    status_.phase = UpdateStatus::Phase::Failed;
    status_.error = error;
    status_.failedDuringInstall = duringInstall;
    status_.detail = std::move(detail);
}

void Updater::check() {
    if (!options_.http) return;
    {
        std::lock_guard lock(mutex_);
        using Phase = UpdateStatus::Phase;
        // One job at a time, and nothing to look for once an update is in place.
        if (status_.phase == Phase::Checking || status_.phase == Phase::Downloading ||
            status_.phase == Phase::Installing || status_.phase == Phase::RestartNeeded) {
            return;
        }
        status_.phase = Phase::Checking;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this](std::stop_token stop) { runCheck(stop); });
}

void Updater::install() {
    if (!options_.http) return;
    {
        std::lock_guard lock(mutex_);
        // Also allowed after a failed attempt, as a retry.
        const bool retry = status_.phase == UpdateStatus::Phase::Failed && status_.failedDuringInstall;
        if ((status_.phase != UpdateStatus::Phase::Available && !retry) || !release_) return;
        status_.phase = UpdateStatus::Phase::Downloading;
        status_.progress = 0.0f;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this](std::stop_token stop) { runInstall(stop); });
}

void Updater::runCheck(std::stop_token stop) {
    const auto response = options_.http->get(options_.apiUrl, kMaxApiResponse, {}, stop);
    if (!response) {
        if (response.error().code == ErrorCode::Canceled) return set(UpdateStatus::Phase::Idle);
        // GitHub answers 404 when there is no release yet or the repository is private.
        const bool nothingPublished = response.error().code == ErrorCode::Rejected;
        return fail(nothingPublished ? UpdateError::NoRelease : UpdateError::Network, response.error().message);
    }

    auto release = parseLatestRelease(asText(*response));
    if (!release) return fail(UpdateError::BadRelease, release.error().message);

    std::lock_guard lock(mutex_);
    status_.latest = release->version;
    status_.notesUrl = release->pageUrl;
    status_.error = UpdateError::None;
    status_.detail.clear();
    if (release->version > options_.current) {
        spdlog::info("Update: version {} is available (running {})", release->version.toString(),
                     options_.current.toString());
        release_ = std::move(*release);
        status_.phase = UpdateStatus::Phase::Available;
    } else {
        spdlog::info("Update: {} is the latest version", options_.current.toString());
        status_.phase = UpdateStatus::Phase::UpToDate;
    }
}

void Updater::runInstall(std::stop_token stop) {
    ReleaseInfo release;
    {
        std::lock_guard lock(mutex_);
        release = *release_;
    }

    const auto archive = options_.http->get(
        release.downloadUrl, kMaxDownload,
        [this](std::uint64_t received, std::uint64_t total) {
            if (total == 0) return;
            std::lock_guard lock(mutex_);
            status_.progress = std::min(1.0f, static_cast<float>(received) / static_cast<float>(total));
        },
        stop);
    if (!archive) {
        if (archive.error().code == ErrorCode::Canceled) return set(UpdateStatus::Phase::Available);
        return fail(UpdateError::Network, archive.error().message, true);
    }

    // The checksum comes from GitHub's API, the file from its download servers.
    // A damaged or swapped file does not get past this point.
    const auto digest = crypto::sha256(*archive);
    if (!digest || toHex(*digest) != release.sha256) {
        return fail(UpdateError::Checksum, "the downloaded file does not match the published checksum", true);
    }
    set(UpdateStatus::Phase::Installing);

    namespace fs = std::filesystem;
    const fs::path staging = options_.workDir / "staging";
    std::error_code ec;
    fs::remove_all(staging, ec);

    const auto files = extractZip(*archive, staging);
    if (!files) return fail(UpdateError::BadArchive, files.error().message, true);
    if (std::find(files->begin(), files->end(), fs::path("VeloPlay.exe")) == files->end()) {
        fs::remove_all(staging, ec);
        return fail(UpdateError::BadArchive, "the archive does not contain VeloPlay.exe", true);
    }

    const auto swapped = swapInFiles(staging, options_.installDir, *files);
    fs::remove_all(staging, ec);
    if (!swapped) return fail(UpdateError::NotWritable, swapped.error().message, true);

    spdlog::info("Update: version {} is installed, restart to use it", release.version.toString());
    set(UpdateStatus::Phase::RestartNeeded);
}

} // namespace veloplay::update
