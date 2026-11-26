#pragma once

namespace veloplay::update {

/// What VeloPlay comes in on one platform: the file of a release, the program
/// inside it, and how the rest of the package is recognised. Both platforms
/// are described here so the tests on Linux go on checking the Windows values;
/// which one this program is, is decided in one place below.
struct PackageLayout {
    enum class Platform { Windows, Linux };
    Platform platform;
    /// The file of a GitHub release that holds the package.
    const char* releaseAsset;
    /// The program, at the top level of the archive and of the folder it runs from.
    const char* programFile;
    /// For messages: "the latest release has no <name> download".
    const char* platformName;
    /// Zip extraction loses the executable bit; it is put back on the programs.
    bool restoresExecutableBit;
    /// The release workflow publishes this file a few minutes after the release
    /// itself. A release without it is not an update yet, and not an error.
    bool mayArriveLater;

    friend bool operator==(const PackageLayout&, const PackageLayout&) = default;
};

inline constexpr PackageLayout kWindowsPackage{
    PackageLayout::Platform::Windows, "VeloPlay-windows-x64.zip", "VeloPlay.exe", "Windows", false, false};
inline constexpr PackageLayout kLinuxPackage{
    PackageLayout::Platform::Linux, "VeloPlay-linux-x64.zip", "VeloPlay", "Linux", true, true};

/// The programs of the Linux package besides `programFile`; they too need the executable bit.
inline constexpr const char* kLinuxHelperPrograms[] = {"veloplay-cli"};

#ifdef _WIN32
inline constexpr const PackageLayout& kThisPackage = kWindowsPackage;
#else
inline constexpr const PackageLayout& kThisPackage = kLinuxPackage;
#endif

} // namespace veloplay::update
