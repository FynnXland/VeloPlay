#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "app/Config.hpp"
#include "update/HttpClient.hpp"
#include "util/Bytes.hpp"
#include "util/Result.hpp"

namespace veloplay::linuxos {

/// The file of trusted certificate authorities on this system, or empty if none
/// is found. The release links a curl and OpenSSL built elsewhere, whose
/// compiled-in path may not exist here. Order: $SSL_CERT_FILE (OpenSSL's own
/// variable, so a user's choice still counts once CAINFO is set explicitly),
/// then the bundles of Debian/Ubuntu/Arch, Fedora/RHEL (two), openSUSE, and
/// Alpine/macOS-style /etc/ssl/cert.pem.
[[nodiscard]] std::string caBundle();

/// A folder of hashed certificates as OpenSSL reads them, or empty.
[[nodiscard]] std::string caDirectory();

/// For tests: what differs from the program's own client.
struct CurlSettings {
    std::string caFile = caBundle();    // CURLOPT_CAINFO; empty leaves curl's default
    std::string caPath = caDirectory(); // CURLOPT_CAPATH; empty leaves curl's default
    std::string noProxy;                // CURLOPT_NOPROXY, e.g. "127.0.0.1" so a test server is not proxied
    long lowSpeedSeconds = 30;          // give up when nothing arrives for this long
};

/// HTTPS with libcurl, with the rules of the Windows client (WinHttpClient):
/// HTTPS only (InvalidArgument otherwise), redirects only to HTTPS, status
/// 200 only (Rejected, "the server answered with HTTP <n>"), bodies above
/// `maxSize` fail with InvalidData, a stop request with Canceled, everything
/// else with Network. Proxies from the environment (https_proxy, no_proxy)
/// apply, like the system settings on Windows. Each call is a transfer of its
/// own; the client may be used from several threads at once.
std::unique_ptr<update::IHttpClient> createCurlHttpClient(std::string userAgent);
std::unique_ptr<update::IHttpClient> createCurlHttpClient(std::string userAgent, CurlSettings settings);

// ---- secrets ------------------------------------------------------------------

/// Where the one key lives that seals every secret of VeloPlay. Never a file.
class IMasterKeyStore {
public:
    virtual ~IMasterKeyStore() = default;
    /// The key, or nullopt when the store answered that it has none yet. An
    /// error when it could not be asked at all (no keyring, locked, the user
    /// cancelled the prompt): then a new key must not be made, it would
    /// replace the one that sealed the pairings already saved.
    virtual Result<std::optional<Bytes>> load() = 0;
    virtual Result<void> save(std::span<const std::uint8_t> key) = 0;
};

inline constexpr std::size_t kMasterKeySize = 32; // AES-256

/// Seals with AES-256-GCM under the store's key: one version byte, a random
/// 12-byte nonce, the ciphertext and the 16-byte tag; the version byte is
/// authenticated too. The key is asked for once and then kept in memory; the
/// first protect() on a system without one makes it. Safe from several threads.
std::unique_ptr<ISecretProtector> createSealingProtector(std::unique_ptr<IMasterKeyStore> keys);

/// The freedesktop Secret Service (GNOME Keyring, KWallet, KeePassXC) through
/// libsecret, which is loaded when first needed, so the program starts on
/// systems without it. The key is an item of the schema "VeloPlay.MasterKey"
/// with application=veloplay and key=`name` in the default collection, as hex.
/// Calls go over D-Bus and wait for the answer, also for an unlock prompt.
/// `name` other than "master" is for tests.
std::unique_ptr<IMasterKeyStore> createSecretServiceKeyStore(std::string name = "master");

/// Removes the item `name` from the Secret Service again (for tests). True if
/// it is gone afterwards.
bool forgetSecretServiceKey(const std::string& name);

/// What the program uses for ISecretProtector on Linux. Without a keyring
/// protect() fails with "no keyring (Secret Service) on this system; pairing
/// cannot be saved"; there is no key file to fall back on (rule 7).
std::unique_ptr<ISecretProtector> createSecretServiceProtector();

} // namespace veloplay::linuxos
