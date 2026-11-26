#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <unistd.h>

#include "linux/LinuxServices.hpp"

using namespace veloplay;

namespace {

/// A keyring in memory. `reachable` false stands for "no Secret Service", `forgets` for one that loses what it got.
struct MemoryKeys final : linuxos::IMasterKeyStore {
    std::optional<Bytes>* key;
    bool* reachable;
    bool forgets = false;
    int saves = 0;

    MemoryKeys(std::optional<Bytes>* key, bool* reachable) : key(key), reachable(reachable) {}

    Result<std::optional<Bytes>> load() override {
        if (!*reachable) return Error{ErrorCode::Crypto, "org.freedesktop.secrets was not provided"};
        return *key;
    }
    Result<void> save(std::span<const std::uint8_t> value) override {
        if (!*reachable) return Error{ErrorCode::Crypto, "org.freedesktop.secrets was not provided"};
        ++saves;
        if (!forgets) *key = Bytes(value.begin(), value.end());
        return {};
    }
};

Bytes bytes(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

} // namespace

TEST(LinuxSecrets, SealsAndOpensAndTheSealedTextHidesTheSecret) {
    std::optional<Bytes> key;
    bool reachable = true;
    const auto protector = linuxos::createSealingProtector(std::make_unique<MemoryKeys>(&key, &reachable));
    const Bytes secret = bytes("rp_key 0123456789abcdef");
    const auto sealed = protector->protect(secret);
    ASSERT_TRUE(sealed) << sealed.error().message;
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->size(), linuxos::kMasterKeySize);
    EXPECT_EQ(sealed->size(), 1 + 12 + secret.size() + 16);
    EXPECT_EQ((*sealed)[0], 1);
    EXPECT_EQ(std::search(sealed->begin(), sealed->end(), secret.begin(), secret.end()), sealed->end());
    const auto opened = protector->unprotect(*sealed);
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(*opened, secret);

    // The same secret twice gives two different texts: the nonce is new each time.
    const auto again = protector->protect(secret);
    ASSERT_TRUE(again);
    EXPECT_NE(*again, *sealed);
    EXPECT_TRUE(protector->protect({}).ok());

    // A second protector (the next start) finds the key and opens what the first sealed.
    const auto later = linuxos::createSealingProtector(std::make_unique<MemoryKeys>(&key, &reachable));
    const auto reopened = later->unprotect(*sealed);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(*reopened, secret);
}

TEST(LinuxSecrets, RefusesWhatWasChangedOrSealedUnderAnotherKey) {
    std::optional<Bytes> key;
    bool reachable = true;
    const auto protector = linuxos::createSealingProtector(std::make_unique<MemoryKeys>(&key, &reachable));
    const Bytes sealed = *protector->protect(bytes("regist_key"));
    for (std::size_t i = 0; i < sealed.size(); ++i) {
        Bytes changed = sealed;
        changed[i] ^= 0x01;
        EXPECT_FALSE(protector->unprotect(changed)) << "byte " << i;
    }
    EXPECT_FALSE(protector->unprotect(Bytes(sealed.begin(), sealed.end() - 1)));
    EXPECT_FALSE(protector->unprotect({}));

    std::optional<Bytes> otherKey;
    const auto other = linuxos::createSealingProtector(std::make_unique<MemoryKeys>(&otherKey, &reachable));
    ASSERT_TRUE(other->protect(bytes("x")));
    const auto foreign = other->unprotect(sealed);
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, ErrorCode::Crypto);
}

// Without a keyring nothing is saved, and above all no key is made while the
// keyring is only out of reach: it would replace the key of the saved pairings.
TEST(LinuxSecrets, MakesNoKeyWhileTheKeyringCannotBeAsked) {
    std::optional<Bytes> key;
    bool reachable = false;
    auto keys = std::make_unique<MemoryKeys>(&key, &reachable);
    MemoryKeys* store = keys.get();
    const auto protector = linuxos::createSealingProtector(std::move(keys));
    const auto refused = protector->protect(bytes("secret"));
    ASSERT_FALSE(refused);
    EXPECT_TRUE(
        refused.error().message.starts_with("no keyring (Secret Service) on this system; pairing cannot be saved"))
        << refused.error().message;
    EXPECT_FALSE(protector->unprotect(Bytes(40, 1)));
    EXPECT_EQ(store->saves, 0);

    // A keyring that is there but has no key yet: only protect makes one, unprotect does not.
    reachable = true;
    const auto reader = linuxos::createSealingProtector(std::make_unique<MemoryKeys>(&key, &reachable));
    EXPECT_FALSE(reader->unprotect(Bytes(40, 1)));
    EXPECT_FALSE(key.has_value());
    EXPECT_TRUE(protector->protect(bytes("secret")).ok()); // the keyring came back
}

TEST(LinuxSecrets, TrustsNoKeyringThatForgetsTheKey) {
    std::optional<Bytes> key;
    bool reachable = true;
    auto keys = std::make_unique<MemoryKeys>(&key, &reachable);
    keys->forgets = true;
    const auto protector = linuxos::createSealingProtector(std::move(keys));
    EXPECT_FALSE(protector->protect(bytes("secret")));
}

// The real Secret Service, only where one runs (a desktop session); CI machines have none.
TEST(LinuxSecrets, KeepsAKeyInTheSecretServiceWhereThereIsOne) {
    const char* bus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!bus || !*bus) GTEST_SKIP() << "no D-Bus session here";
    const std::string name = "test-" + std::to_string(::getpid());
    auto store = linuxos::createSecretServiceKeyStore(name);
    const auto before = store->load();
    if (!before) GTEST_SKIP() << "no Secret Service here: " << before.error().message;
    EXPECT_FALSE(before.value().has_value());

    const auto protector = linuxos::createSealingProtector(linuxos::createSecretServiceKeyStore(name));
    const auto sealed = protector->protect(bytes("secret"));
    ASSERT_TRUE(sealed) << sealed.error().message;
    const auto later = linuxos::createSealingProtector(linuxos::createSecretServiceKeyStore(name));
    const auto opened = later->unprotect(*sealed);
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(*opened, bytes("secret"));
    EXPECT_TRUE(linuxos::forgetSecretServiceKey(name));
}

// Without a session bus the real store says so as an error, never as "no key yet".
TEST(LinuxSecrets, TheRealStoreReportsAMissingKeyringAsAnError) {
    const char* bus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus && *bus) GTEST_SKIP() << "a D-Bus session is here";
    const char* display = std::getenv("DISPLAY");
    if (display && *display) GTEST_SKIP() << "D-Bus could be started for this display";
    // An item of its own: a session bus may be reachable anyway, and the real master key must stay untouched.
    const std::string name = "test-absent-" + std::to_string(::getpid());
    const auto protector = linuxos::createSealingProtector(linuxos::createSecretServiceKeyStore(name));
    const auto refused = protector->protect(bytes("secret"));
    if (refused) {
        linuxos::forgetSecretServiceKey(name);
        GTEST_SKIP() << "a Secret Service answered after all";
    }
    EXPECT_TRUE(refused.error().message.starts_with("no keyring (Secret Service) on this system"))
        << refused.error().message;
}
