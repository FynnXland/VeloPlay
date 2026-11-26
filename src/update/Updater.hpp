#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "update/HttpClient.hpp"
#include "update/Release.hpp"
#include "update/Version.hpp"
#include "util/Result.hpp"

// Self-update from GitHub releases.
//
//   check:   ask GitHub for the latest release and compare version numbers
//   install: download the archive, verify its SHA-256, unpack it next to the
//            program and swap the files in
//
// Windows does not allow overwriting a program that is running, but it allows
// renaming it. So each old file is renamed to "<name>.old", the new file takes
// its place, and the next start deletes the ".old" files. No helper program and
// no administrator rights are needed as long as the folder is writable.
// Linux allows replacing a running program as well; the same way works there,
// and the programs get their executable bit back, which unpacking loses.

namespace veloplay::update {

enum class UpdateError {
    None,
    Network,       // no answer from GitHub
    NoRelease,     // nothing published (or the repository is not public)
    BadRelease,    // release data incomplete or not from our repository
    Checksum,      // download does not match the published SHA-256
    BadArchive,    // not a zip, unsafe paths, or the client is not inside
    NotWritable,   // the program folder cannot be changed
};

struct UpdateStatus {
    enum class Phase {
        Idle,
        Checking,
        UpToDate,
        Available,      // a newer version exists; waits for the user
        Downloading,
        Installing,
        RestartNeeded,  // files are in place; the running program is still the old one
        Failed,
    };
    Phase phase = Phase::Idle;
    Version current;
    Version latest;
    std::string notesUrl;
    float progress = 0.0f; // 0..1 while downloading
    UpdateError error = UpdateError::None;
    /// True if the failure happened after the user asked to install. A failed
    /// check at startup is not worth interrupting anyone for.
    bool failedDuringInstall = false;
    std::string detail;    // technical message for the log line
};

/// Replaces the files in `installDir` with those in `stagingDir`. `files` are
/// paths relative to both. On failure everything already swapped is put back.
Result<void> swapInFiles(const std::filesystem::path& stagingDir, const std::filesystem::path& installDir,
                         const std::vector<std::filesystem::path>& files);

/// Gives the programs of the Linux package among `files` (paths relative to
/// `folder`) the mode 0755. Unpacking a zip leaves every file without it.
Result<void> restoreExecutableBits(const std::filesystem::path& folder, const std::vector<std::filesystem::path>& files);

/// Deletes the "*.old" files a previous update left behind.
void removeLeftovers(const std::filesystem::path& installDir);

class Updater {
public:
    struct Options {
        Version current;
        std::filesystem::path installDir; // folder that holds the program (VeloPlay.exe, VeloPlay)
        std::filesystem::path workDir;    // scratch space for the unpacked download
        IHttpClient* http = nullptr;      // must outlive the updater
        std::string apiUrl = latestReleaseApiUrl();
        PackageLayout package = kThisPackage; // which file of a release is ours
    };

    explicit Updater(Options options);
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    /// Looks for a newer release in the background.
    void check();

    /// Downloads and installs the release found by check(). Valid while the
    /// status is Available, and as a retry after a failed installation.
    void install();

    [[nodiscard]] UpdateStatus status() const;

private:
    void set(UpdateStatus::Phase phase);
    void fail(UpdateError error, std::string detail, bool duringInstall = false);
    void runCheck(std::stop_token stop);
    void runInstall(std::stop_token stop);

    Options options_;
    mutable std::mutex mutex_;
    UpdateStatus status_;
    std::optional<ReleaseInfo> release_;
    std::jthread worker_;
};

} // namespace veloplay::update
