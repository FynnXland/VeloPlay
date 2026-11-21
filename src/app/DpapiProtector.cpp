#include "app/Config.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

namespace veloplay {

namespace {

// Extra entropy only keeps other programs from decrypting the blob by
// accident. It is not a secret; the protection comes from the user account.
constexpr char kEntropy[] = "VeloPlay.Credentials.v1";

DATA_BLOB entropyBlob() {
    DATA_BLOB blob;
    blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    blob.cbData = sizeof(kEntropy) - 1;
    return blob;
}

class DpapiProtector final : public ISecretProtector {
public:
    Result<Bytes> protect(std::span<const std::uint8_t> plaintext) override {
        DATA_BLOB input{static_cast<DWORD>(plaintext.size()), const_cast<BYTE*>(plaintext.data())};
        DATA_BLOB entropy = entropyBlob();
        DATA_BLOB output{};
        if (!CryptProtectData(&input, L"VeloPlay console keys", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            return Error{ErrorCode::Crypto, "CryptProtectData failed with error " + std::to_string(GetLastError())};
        }
        Bytes result(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return result;
    }

    Result<Bytes> unprotect(std::span<const std::uint8_t> ciphertext) override {
        DATA_BLOB input{static_cast<DWORD>(ciphertext.size()), const_cast<BYTE*>(ciphertext.data())};
        DATA_BLOB entropy = entropyBlob();
        DATA_BLOB output{};
        if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            return Error{ErrorCode::Crypto, "CryptUnprotectData failed with error " + std::to_string(GetLastError())};
        }
        Bytes result(output.pbData, output.pbData + output.cbData);
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return result;
    }
};

} // namespace

std::unique_ptr<ISecretProtector> createDpapiProtector() { return std::make_unique<DpapiProtector>(); }

} // namespace veloplay
