// Secrets on Linux: sealed with AES-256-GCM under one master key, and that key
// in the desktop's keyring (the freedesktop Secret Service). The keyring ties
// the key to the user's login as DPAPI does on Windows. Without a keyring the
// secrets are not saved at all; a key file next to them would protect nothing.
#include "linux/LinuxServices.hpp"

#include <dlfcn.h>

#include <mutex>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

// Only for the types and the declarations of the functions looked up at run
// time; nothing of libsecret or glib is linked.
#include <libsecret/secret.h>

namespace veloplay::linuxos {

namespace {

constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagSize = 16;
constexpr char kNoKeyring[] = "no keyring (Secret Service) on this system; pairing cannot be saved";
constexpr char kCannotRead[] = "no keyring (Secret Service) on this system; saved pairings cannot be read";

// ---- AES-256-GCM ---------------------------------------------------------------

struct CipherCloser {
    void operator()(EVP_CIPHER_CTX* context) const { EVP_CIPHER_CTX_free(context); }
};
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, CipherCloser>;

Error cryptoError(const char* what) {
    return Error{ErrorCode::Crypto, std::string(what) + " failed"};
}

Result<Bytes> seal(std::span<const std::uint8_t> key, std::span<const std::uint8_t> plaintext) {
    Bytes out(1 + kNonceSize + plaintext.size() + kTagSize);
    out[0] = kVersion;
    std::uint8_t* nonce = out.data() + 1;
    if (RAND_bytes(nonce, static_cast<int>(kNonceSize)) != 1) return cryptoError("RAND_bytes");
    const Cipher context(EVP_CIPHER_CTX_new());
    int written = 0;
    int last = 0;
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) != 1 ||
        EVP_EncryptUpdate(context.get(), nullptr, &written, out.data(), 1) != 1 || // the version byte, authenticated
        EVP_EncryptUpdate(context.get(), out.data() + 1 + kNonceSize, &written, plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(context.get(), out.data() + 1 + kNonceSize + written, &last) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize),
                            out.data() + out.size() - kTagSize) != 1) {
        return cryptoError("AES-GCM sealing");
    }
    return out;
}

Result<Bytes> open(std::span<const std::uint8_t> key, std::span<const std::uint8_t> sealed) {
    if (sealed.size() < 1 + kNonceSize + kTagSize) return Error{ErrorCode::Crypto, "the sealed secret is too short"};
    if (sealed[0] != kVersion) {
        return Error{ErrorCode::Crypto, "the sealed secret has an unknown version " + std::to_string(sealed[0])};
    }
    const std::uint8_t* nonce = sealed.data() + 1;
    const std::span<const std::uint8_t> body =
        sealed.subspan(1 + kNonceSize, sealed.size() - 1 - kNonceSize - kTagSize);
    Bytes tag(sealed.end() - kTagSize, sealed.end());
    Bytes out(body.size() + 1);
    const Cipher context(EVP_CIPHER_CTX_new());
    int written = 0;
    int last = 0;
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) != 1 ||
        EVP_DecryptUpdate(context.get(), nullptr, &written, sealed.data(), 1) != 1 ||
        EVP_DecryptUpdate(context.get(), out.data(), &written, body.data(), static_cast<int>(body.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize), tag.data()) != 1) {
        return cryptoError("AES-GCM opening");
    }
    // The tag decides: changed, cut or sealed under another key, nothing comes out.
    if (EVP_DecryptFinal_ex(context.get(), out.data() + written, &last) != 1) {
        OPENSSL_cleanse(out.data(), out.size());
        return Error{ErrorCode::Crypto, "the sealed secret does not match its key (changed, or sealed elsewhere)"};
    }
    out.resize(static_cast<std::size_t>(written + last));
    return out;
}

class SealingProtector final : public ISecretProtector {
public:
    explicit SealingProtector(std::unique_ptr<IMasterKeyStore> keys) : keys_(std::move(keys)) {}

    ~SealingProtector() override {
        if (!key_.empty()) OPENSSL_cleanse(key_.data(), key_.size());
    }

    Result<Bytes> protect(std::span<const std::uint8_t> plaintext) override {
        std::lock_guard lock(mutex_);
        if (auto ready = loadKey(true); !ready) return ready.error();
        return seal(key_, plaintext);
    }

    Result<Bytes> unprotect(std::span<const std::uint8_t> ciphertext) override {
        std::lock_guard lock(mutex_);
        if (auto ready = loadKey(false); !ready) return ready.error();
        return open(key_, ciphertext);
    }

private:
    /// Fills key_. Only protect() may make a key, and only when the store said it has none.
    Result<void> loadKey(bool mayCreate) {
        if (!key_.empty()) return {};
        // What the store says comes after the sentence the log and the interface read first.
        const auto unreachable = [mayCreate](const Error& error) {
            return Error{ErrorCode::Crypto,
                         std::string(mayCreate ? kNoKeyring : kCannotRead) + " (" + error.message + ")"};
        };
        if (!keys_) return unreachable(Error{ErrorCode::Crypto, "no key store"});
        auto stored = keys_->load();
        if (!stored) return unreachable(stored.error());
        if (stored.value()) {
            if (stored.value()->size() != kMasterKeySize) {
                return Error{ErrorCode::Crypto, "the master key in the keyring has the wrong length"};
            }
            key_ = std::move(*stored.value());
            return {};
        }
        if (!mayCreate) return Error{ErrorCode::Crypto, "there is no master key in the keyring yet"};

        Bytes fresh(kMasterKeySize);
        if (RAND_bytes(fresh.data(), static_cast<int>(fresh.size())) != 1) return cryptoError("RAND_bytes");
        if (auto saved = keys_->save(fresh); !saved) {
            OPENSSL_cleanse(fresh.data(), fresh.size());
            return unreachable(saved.error());
        }
        // Read back: a keyring that accepts and forgets would leave secrets nobody can open again.
        auto back = keys_->load();
        if (!back || !back.value() || *back.value() != fresh) {
            OPENSSL_cleanse(fresh.data(), fresh.size());
            return Error{ErrorCode::Crypto, "the keyring did not keep the master key"};
        }
        spdlog::info("Secrets: a new master key is in the keyring");
        key_ = std::move(fresh);
        return {};
    }

    std::unique_ptr<IMasterKeyStore> keys_;
    std::mutex mutex_;
    Bytes key_;
};

// ---- libsecret, loaded at run time -----------------------------------------------

struct SecretLibrary {
    decltype(&secret_password_store_sync) store = nullptr;
    decltype(&secret_password_lookup_sync) lookup = nullptr;
    decltype(&secret_password_clear_sync) clear = nullptr;
    decltype(&secret_password_free) free = nullptr;
    decltype(&secret_password_search_sync) search = nullptr;
    decltype(&g_error_free) freeError = nullptr;
    decltype(&g_list_free_full) freeList = nullptr;
    decltype(&g_object_unref) unref = nullptr;
    std::string problem; // why it is not there

    [[nodiscard]] bool usable() const { return problem.empty(); }
};

/// Loaded once and never unloaded: glib does not like to leave a process.
const SecretLibrary& secretLibrary() {
    static const SecretLibrary library = [] {
        SecretLibrary loaded;
        void* secret = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        // libsecret brings glib and gobject along; these only find the copies already there.
        void* glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        void* gobject = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!secret || !glib || !gobject) {
            loaded.problem = "libsecret-1.so.0 cannot be loaded";
            return loaded;
        }
        loaded.store = reinterpret_cast<decltype(loaded.store)>(dlsym(secret, "secret_password_store_sync"));
        loaded.lookup = reinterpret_cast<decltype(loaded.lookup)>(dlsym(secret, "secret_password_lookup_sync"));
        loaded.clear = reinterpret_cast<decltype(loaded.clear)>(dlsym(secret, "secret_password_clear_sync"));
        loaded.free = reinterpret_cast<decltype(loaded.free)>(dlsym(secret, "secret_password_free"));
        loaded.search = reinterpret_cast<decltype(loaded.search)>(dlsym(secret, "secret_password_search_sync"));
        loaded.freeError = reinterpret_cast<decltype(loaded.freeError)>(dlsym(glib, "g_error_free"));
        loaded.freeList = reinterpret_cast<decltype(loaded.freeList)>(dlsym(glib, "g_list_free_full"));
        loaded.unref = reinterpret_cast<decltype(loaded.unref)>(dlsym(gobject, "g_object_unref"));
        // search_sync came with libsecret 0.19 (2019); without it a locked keyring could not be told from an empty one.
        if (!loaded.store || !loaded.lookup || !loaded.clear || !loaded.free || !loaded.search || !loaded.freeError ||
            !loaded.freeList || !loaded.unref) {
            loaded.problem = "libsecret-1.so.0 lacks a function VeloPlay needs";
        }
        return loaded;
    }();
    return library;
}

const SecretSchema* masterKeySchema() {
    static const SecretSchema schema = [] {
        SecretSchema made{};
        made.name = "VeloPlay.MasterKey";
        made.flags = SECRET_SCHEMA_NONE;
        made.attributes[0] = {"application", SECRET_SCHEMA_ATTRIBUTE_STRING};
        made.attributes[1] = {"key", SECRET_SCHEMA_ATTRIBUTE_STRING};
        made.attributes[2] = {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING};
        return made;
    }();
    return &schema;
}

/// The message of a GError, which is freed.
std::string takeMessage(const SecretLibrary& library, GError* error) {
    std::string message = error && error->message ? error->message : "unknown error";
    if (error) library.freeError(error);
    return message;
}

class SecretServiceKeyStore final : public IMasterKeyStore {
public:
    explicit SecretServiceKeyStore(std::string name) : name_(std::move(name)) {}

    Result<std::optional<Bytes>> load() override {
        const SecretLibrary& library = secretLibrary();
        if (!library.usable()) return Error{ErrorCode::Crypto, library.problem};
        GError* error = nullptr;
        gchar* text = library.lookup(masterKeySchema(), nullptr, &error, "application", "veloplay", "key",
                                     name_.c_str(), static_cast<char*>(nullptr));
        if (error) {
            if (text) library.free(text);
            // No session bus, no service on it, a locked keyring the user did not open.
            return Error{ErrorCode::Crypto, takeMessage(library, error)};
        }
        if (!text) {
            // Nothing came back. That is also the answer when the keyring is locked and the user closed the
            // prompt to open it, without an error. So look once more without opening anything: a locked item
            // of ours means the key is there, and a new one would replace it and orphan the saved pairings.
            GList* found = library.search(masterKeySchema(), SECRET_SEARCH_ALL, nullptr, &error, "application",
                                          "veloplay", "key", name_.c_str(), static_cast<char*>(nullptr));
            const bool exists = found != nullptr;
            if (found) library.freeList(found, library.unref);
            if (error) return Error{ErrorCode::Crypto, takeMessage(library, error)};
            if (exists) return Error{ErrorCode::Crypto, "the keyring is locked and was not opened"};
            return std::optional<Bytes>{}; // asked and answered: there is none
        }
        std::optional<Bytes> key = fromHex(text);
        library.free(text); // wipes the memory before freeing it
        if (!key) return Error{ErrorCode::Crypto, "the master key in the keyring is not readable"};
        return key;
    }

    Result<void> save(std::span<const std::uint8_t> key) override {
        const SecretLibrary& library = secretLibrary();
        if (!library.usable()) return Error{ErrorCode::Crypto, library.problem};
        std::string text = toHex(key);
        GError* error = nullptr;
        const gboolean stored =
            library.store(masterKeySchema(), SECRET_COLLECTION_DEFAULT, "VeloPlay master key", text.c_str(), nullptr,
                          &error, "application", "veloplay", "key", name_.c_str(), static_cast<char*>(nullptr));
        OPENSSL_cleanse(text.data(), text.size());
        if (error || !stored) {
            return Error{ErrorCode::Crypto, takeMessage(library, error)};
        }
        return {};
    }

private:
    std::string name_;
};

} // namespace

std::unique_ptr<ISecretProtector> createSealingProtector(std::unique_ptr<IMasterKeyStore> keys) {
    return std::make_unique<SealingProtector>(std::move(keys));
}

std::unique_ptr<IMasterKeyStore> createSecretServiceKeyStore(std::string name) {
    return std::make_unique<SecretServiceKeyStore>(std::move(name));
}

bool forgetSecretServiceKey(const std::string& name) {
    const SecretLibrary& library = secretLibrary();
    if (!library.usable()) return false;
    GError* error = nullptr;
    library.clear(masterKeySchema(), nullptr, &error, "application", "veloplay", "key", name.c_str(),
                  static_cast<char*>(nullptr));
    if (error) {
        takeMessage(library, error);
        return false;
    }
    return true;
}

std::unique_ptr<ISecretProtector> createSecretServiceProtector() {
    return createSealingProtector(createSecretServiceKeyStore());
}

} // namespace veloplay::linuxos
