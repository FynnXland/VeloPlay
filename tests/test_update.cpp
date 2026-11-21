#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <sstream>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "MockConsole.hpp"
#include "protocol/Crypto.hpp"
#include "update/Release.hpp"
#include "update/Updater.hpp"
#include "update/Version.hpp"
#include "update/Zip.hpp"

using namespace veloplay;
using namespace veloplay::update;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/// Builds a zip archive in memory. Names are written as given, safe or not.
Bytes makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    EXPECT_TRUE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, content] : files) {
        EXPECT_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_BEST_SPEED));
    }
    void* data = nullptr;
    std::size_t size = 0;
    EXPECT_TRUE(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
    Bytes result(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
    mz_free(data);
    mz_zip_writer_end(&zip);
    return result;
}

std::string sha256Hex(const Bytes& data) { return toHex(*crypto::sha256(data)); }

fs::path freshDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "veloplay_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

const std::string kBase = "https://github.com/FynnXland/VeloPlay/releases/";
const std::string kAssetUrl = kBase + "download/v0.9.0/VeloPlay-windows-x64.zip";

json releaseJson(const std::string& tag, const std::string& digestHex, const std::string& url = kAssetUrl) {
    return json{{"tag_name", tag},
                {"html_url", kBase + "tag/" + tag},
                {"draft", false},
                {"prerelease", false},
                {"assets", json::array({json{{"name", "notes.txt"}, {"browser_download_url", kBase + "download/x/notes.txt"}},
                                        json{{"name", "VeloPlay-windows-x64.zip"},
                                             {"browser_download_url", url},
                                             {"size", 1234},
                                             {"digest", "sha256:" + digestHex}}})}};
}

/// Serves prepared answers from memory and records what was asked for.
class FakeHttp final : public IHttpClient {
public:
    std::map<std::string, Bytes> pages;
    std::map<std::string, Error> errors;
    std::vector<std::string> requested;

    void add(const std::string& url, std::string_view body) { pages[url] = Bytes(body.begin(), body.end()); }

    Result<Bytes> get(const std::string& url, std::uint64_t maxSize, const Progress& progress, std::stop_token) override {
        requested.push_back(url);
        if (const auto error = errors.find(url); error != errors.end()) return error->second;
        const auto page = pages.find(url);
        if (page == pages.end()) return Error{ErrorCode::Rejected, "the server answered with HTTP 404"};
        if (page->second.size() > maxSize) return Error{ErrorCode::InvalidData, "too large"};
        if (progress) progress(page->second.size(), page->second.size());
        return page->second;
    }
};

const std::string kApi = "https://api.example/releases/latest";

struct UpdateFixture {
    FakeHttp http;
    fs::path install;
    fs::path work;
    std::unique_ptr<Updater> updater;

    explicit UpdateFixture(const char* name, Version current = {0, 2, 0}) {
        install = freshDir((std::string(name) + "_install").c_str());
        work = freshDir((std::string(name) + "_work").c_str());
        writeFile(install / "VeloPlay.exe", "old program");
        writeFile(install / "SDL3.dll", "old library");
        writeFile(install / "notes-from-user.txt", "keep me");

        Updater::Options options;
        options.current = current;
        options.installDir = install;
        options.workDir = work;
        options.http = &http;
        options.apiUrl = kApi;
        updater = std::make_unique<Updater>(std::move(options));
    }

    void publish(const std::string& tag, const Bytes& archive, const std::string& digest = {}) {
        http.add(kApi, releaseJson(tag, digest.empty() ? sha256Hex(archive) : digest).dump());
        http.pages[kAssetUrl] = archive;
    }

    UpdateStatus::Phase waitWhile(std::initializer_list<UpdateStatus::Phase> busy) {
        test::waitFor([&] {
            const auto phase = updater->status().phase;
            return std::find(busy.begin(), busy.end(), phase) == busy.end();
        });
        return updater->status().phase;
    }

    UpdateStatus::Phase checkNow() {
        updater->check();
        return waitWhile({UpdateStatus::Phase::Checking});
    }

    UpdateStatus::Phase installNow() {
        updater->install();
        return waitWhile({UpdateStatus::Phase::Downloading, UpdateStatus::Phase::Installing});
    }
};

const Bytes kNewArchive = makeZip({{"VeloPlay.exe", "new program"}, {"SDL3.dll", "new library"}, {"extra/readme.txt", "hello"}});

} // namespace

// ---- version numbers -----------------------------------------------------

TEST(Version, ParsesWithAndWithoutPrefix) {
    EXPECT_EQ(Version::parse("1.2.3"), (Version{1, 2, 3}));
    EXPECT_EQ(Version::parse("v0.10.0"), (Version{0, 10, 0}));
    EXPECT_EQ((Version{0, 2, 0}).toString(), "0.2.0");
}

TEST(Version, RejectsEverythingElse) {
    for (const char* text : {"", "1", "1.2", "1.2.3.4", "1.2.x", "v", "1..3", "1.2.3-beta", "1.2.3 ", "-1.2.3", "latest"}) {
        EXPECT_FALSE(Version::parse(text)) << text;
    }
}

TEST(Version, ComparesNumericallyNotAsText) {
    EXPECT_LT((Version{0, 9, 0}), (Version{0, 10, 0}));
    EXPECT_LT((Version{0, 2, 9}), (Version{0, 3, 0}));
    EXPECT_LT((Version{0, 99, 99}), (Version{1, 0, 0}));
    EXPECT_EQ((Version{1, 2, 3}), (Version{1, 2, 3}));
}

// ---- release data ----------------------------------------------------------

TEST(Release, ParsesGitHubAnswer) {
    const std::string digest(64, 'a');
    const auto release = parseLatestRelease(releaseJson("v0.9.0", digest).dump());
    ASSERT_TRUE(release) << release.error().message;
    EXPECT_EQ(release->version, (Version{0, 9, 0}));
    EXPECT_EQ(release->downloadUrl, kAssetUrl);
    EXPECT_EQ(release->sha256, digest);
    EXPECT_EQ(release->size, 1234u);
    EXPECT_EQ(release->pageUrl, kBase + "tag/v0.9.0");
}

TEST(Release, RefusesDownloadsFromElsewhere) {
    const std::string digest(64, 'a');
    for (const char* url : {"https://evil.example/VeloPlay-windows-x64.zip",
                            "http://github.com/FynnXland/VeloPlay/releases/download/v0.9.0/VeloPlay-windows-x64.zip",
                            "https://github.com/Someone/Else/releases/download/v0.9.0/VeloPlay-windows-x64.zip",
                            "https://github.com.evil.example/FynnXland/VeloPlay/releases/download/x.zip"}) {
        EXPECT_FALSE(parseLatestRelease(releaseJson("v0.9.0", digest, url).dump())) << url;
    }
}

TEST(Release, NeedsVersionArchiveAndChecksum) {
    const std::string digest(64, 'a');
    EXPECT_FALSE(parseLatestRelease("not json"));
    EXPECT_FALSE(parseLatestRelease("[]"));
    EXPECT_FALSE(parseLatestRelease(releaseJson("nightly", digest).dump()));
    EXPECT_FALSE(parseLatestRelease(releaseJson("v0.9.0", "abc").dump()));                   // digest too short
    EXPECT_FALSE(parseLatestRelease(releaseJson("v0.9.0", std::string(64, 'Z')).dump()));    // not hex

    json noArchive = releaseJson("v0.9.0", digest);
    noArchive["assets"].erase(1);
    EXPECT_FALSE(parseLatestRelease(noArchive.dump()));

    json noDigest = releaseJson("v0.9.0", digest);
    noDigest["assets"][1].erase("digest");
    EXPECT_FALSE(parseLatestRelease(noDigest.dump()));
}

// ---- zip ---------------------------------------------------------------------

TEST(Zip, PathSafetyRules) {
    for (const char* name : {"VeloPlay.exe", "sub/dir/file.dll", "a..b/c", "name with space.txt"}) {
        EXPECT_TRUE(isSafeArchivePath(name)) << name;
    }
    for (const char* name : {"", "../evil.exe", "a/../../evil", "a\\..\\evil", "/etc/passwd", "\\windows\\x", "C:/x",
                             "C:evil", "file.txt:stream", ".."}) {
        EXPECT_FALSE(isSafeArchivePath(name)) << name;
    }
    EXPECT_FALSE(isSafeArchivePath(std::string(300, 'a')));
}

TEST(Zip, ExtractsFilesAndFolders) {
    const fs::path dir = freshDir("zip_extract");
    const auto files = extractZip(kNewArchive, dir);
    ASSERT_TRUE(files) << files.error().message;
    EXPECT_EQ(files->size(), 3u);
    EXPECT_EQ(readFile(dir / "VeloPlay.exe"), "new program");
    EXPECT_EQ(readFile(dir / "extra" / "readme.txt"), "hello");
}

TEST(Zip, RefusesArchivesThatWriteOutsideTheFolder) {
    const fs::path dir = freshDir("zip_slip");
    const Bytes archive = makeZip({{"VeloPlay.exe", "fine"}, {"../escaped.txt", "gotcha"}});
    const auto files = extractZip(archive, dir / "inner");
    ASSERT_FALSE(files);
    EXPECT_EQ(files.error().code, ErrorCode::InvalidData);
    EXPECT_FALSE(fs::exists(dir / "escaped.txt"));
    EXPECT_FALSE(fs::exists(dir / "inner" / "VeloPlay.exe")); // nothing at all was written
}

TEST(Zip, RefusesGarbage) {
    const Bytes garbage{'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p'};
    EXPECT_FALSE(extractZip(garbage, freshDir("zip_garbage")));
    EXPECT_FALSE(extractZip(Bytes{}, freshDir("zip_empty")));
}

// ---- file swap -----------------------------------------------------------------

TEST(SwapInFiles, ReplacesFilesAndKeepsTheOldOnesAside) {
    const fs::path staging = freshDir("swap_staging");
    const fs::path install = freshDir("swap_install");
    writeFile(staging / "VeloPlay.exe", "new");
    writeFile(staging / "sub" / "added.dll", "added");
    writeFile(install / "VeloPlay.exe", "old");
    writeFile(install / "VeloPlay.exe.old", "leftover of an earlier update");

    ASSERT_TRUE(swapInFiles(staging, install, {"VeloPlay.exe", fs::path("sub") / "added.dll"}));
    EXPECT_EQ(readFile(install / "VeloPlay.exe"), "new");
    EXPECT_EQ(readFile(install / "VeloPlay.exe.old"), "old");
    EXPECT_EQ(readFile(install / "sub" / "added.dll"), "added");

    removeLeftovers(install);
    EXPECT_FALSE(fs::exists(install / "VeloPlay.exe.old"));
    EXPECT_TRUE(fs::exists(install / "VeloPlay.exe"));
}

TEST(SwapInFiles, RollsBackWhenAFileIsMissing) {
    const fs::path staging = freshDir("swap_fail_staging");
    const fs::path install = freshDir("swap_fail_install");
    writeFile(staging / "VeloPlay.exe", "new");
    writeFile(install / "VeloPlay.exe", "old");
    writeFile(install / "SDL3.dll", "old library");

    // SDL3.dll is announced but not in the staging folder.
    const auto swapped = swapInFiles(staging, install, {"VeloPlay.exe", "SDL3.dll"});
    ASSERT_FALSE(swapped);
    EXPECT_EQ(readFile(install / "VeloPlay.exe"), "old");
    EXPECT_EQ(readFile(install / "SDL3.dll"), "old library");
    EXPECT_FALSE(fs::exists(install / "VeloPlay.exe.old"));
}

// ---- whole flow ------------------------------------------------------------------

TEST(Updater, ReportsUpToDate) {
    UpdateFixture f("up_to_date");
    f.publish("v0.2.0", kNewArchive);
    EXPECT_EQ(f.checkNow(), UpdateStatus::Phase::UpToDate);

    f.publish("v0.1.9", kNewArchive); // older than what is running
    EXPECT_EQ(f.checkNow(), UpdateStatus::Phase::UpToDate);
    EXPECT_EQ(f.http.requested.size(), 2u); // never downloaded the archive
}

TEST(Updater, InstallsANewerVersion) {
    UpdateFixture f("install");
    f.publish("v0.9.0", kNewArchive);

    ASSERT_EQ(f.checkNow(), UpdateStatus::Phase::Available);
    EXPECT_EQ(f.updater->status().latest, (Version{0, 9, 0}));
    EXPECT_EQ(readFile(f.install / "VeloPlay.exe"), "old program"); // nothing happens without consent

    ASSERT_EQ(f.installNow(), UpdateStatus::Phase::RestartNeeded) << f.updater->status().detail;
    EXPECT_EQ(readFile(f.install / "VeloPlay.exe"), "new program");
    EXPECT_EQ(readFile(f.install / "SDL3.dll"), "new library");
    EXPECT_EQ(readFile(f.install / "extra" / "readme.txt"), "hello");
    EXPECT_EQ(readFile(f.install / "VeloPlay.exe.old"), "old program"); // the running program stays intact
    EXPECT_EQ(readFile(f.install / "notes-from-user.txt"), "keep me");  // files we do not ship are left alone
    EXPECT_FALSE(fs::exists(f.work / "staging"));

    // Checking again must not undo the "restart needed" state.
    f.updater->check();
    EXPECT_EQ(f.updater->status().phase, UpdateStatus::Phase::RestartNeeded);
}

TEST(Updater, RejectsDownloadWithWrongChecksum) {
    UpdateFixture f("checksum");
    f.publish("v0.9.0", kNewArchive, std::string(64, '0'));
    ASSERT_EQ(f.checkNow(), UpdateStatus::Phase::Available);

    ASSERT_EQ(f.installNow(), UpdateStatus::Phase::Failed);
    const UpdateStatus status = f.updater->status();
    EXPECT_EQ(status.error, UpdateError::Checksum);
    EXPECT_TRUE(status.failedDuringInstall);
    EXPECT_EQ(readFile(f.install / "VeloPlay.exe"), "old program");
    EXPECT_FALSE(fs::exists(f.install / "VeloPlay.exe.old"));
}

TEST(Updater, RejectsArchiveWithoutTheProgramOrWithUnsafePaths) {
    {
        UpdateFixture f("no_program");
        f.publish("v0.9.0", makeZip({{"readme.txt", "no program inside"}}));
        ASSERT_EQ(f.checkNow(), UpdateStatus::Phase::Available);
        ASSERT_EQ(f.installNow(), UpdateStatus::Phase::Failed);
        EXPECT_EQ(f.updater->status().error, UpdateError::BadArchive);
        EXPECT_EQ(readFile(f.install / "VeloPlay.exe"), "old program");
    }
    {
        UpdateFixture f("unsafe");
        f.publish("v0.9.0", makeZip({{"VeloPlay.exe", "new"}, {"../../outside.txt", "x"}}));
        ASSERT_EQ(f.checkNow(), UpdateStatus::Phase::Available);
        ASSERT_EQ(f.installNow(), UpdateStatus::Phase::Failed);
        EXPECT_EQ(f.updater->status().error, UpdateError::BadArchive);
        EXPECT_EQ(readFile(f.install / "VeloPlay.exe"), "old program");
    }
}

TEST(Updater, DistinguishesNothingPublishedFromNoNetwork) {
    UpdateFixture f("errors");
    EXPECT_EQ(f.checkNow(), UpdateStatus::Phase::Failed); // 404: no release, or the repository is private
    EXPECT_EQ(f.updater->status().error, UpdateError::NoRelease);
    EXPECT_FALSE(f.updater->status().failedDuringInstall);

    f.http.errors.insert({kApi, Error{ErrorCode::Network, "no route to host"}});
    EXPECT_EQ(f.checkNow(), UpdateStatus::Phase::Failed);
    EXPECT_EQ(f.updater->status().error, UpdateError::Network);

    f.http.errors.clear();
    f.http.add(kApi, R"({"tag_name":"v9.9.9","assets":[]})");
    EXPECT_EQ(f.checkNow(), UpdateStatus::Phase::Failed);
    EXPECT_EQ(f.updater->status().error, UpdateError::BadRelease);
}

TEST(Updater, FailedDownloadCanBeRetried) {
    UpdateFixture f("retry");
    f.publish("v0.9.0", kNewArchive);
    ASSERT_EQ(f.checkNow(), UpdateStatus::Phase::Available);

    f.http.errors.insert({kAssetUrl, Error{ErrorCode::Network, "connection reset"}});
    ASSERT_EQ(f.installNow(), UpdateStatus::Phase::Failed);
    EXPECT_EQ(f.updater->status().error, UpdateError::Network);
    EXPECT_TRUE(f.updater->status().failedDuringInstall);

    f.http.errors.clear();
    ASSERT_EQ(f.installNow(), UpdateStatus::Phase::RestartNeeded);
    EXPECT_EQ(readFile(f.install / "VeloPlay.exe"), "new program");
}

TEST(Updater, InstallWithoutAFoundReleaseDoesNothing) {
    UpdateFixture f("no_release");
    f.updater->install();
    EXPECT_EQ(f.updater->status().phase, UpdateStatus::Phase::Idle);
    EXPECT_TRUE(f.http.requested.empty());
}
