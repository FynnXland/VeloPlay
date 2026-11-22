#include "render/ShaderCompiler.hpp"

#include <map>
#include <mutex>
#include <tuple>

#include <d3dcompiler.h>

namespace veloplay::render {

bool compileShader(const char* source, std::size_t length, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& code,
                   std::string& error) {
    using Key = std::tuple<const char*, std::size_t, std::string>;
    static std::mutex mutex;
    static std::map<Key, Microsoft::WRL::ComPtr<ID3DBlob>> compiled;
    const Key key{source, length, target};
    {
        std::lock_guard lock(mutex);
        if (const auto found = compiled.find(key); found != compiled.end()) {
            code = found->second;
            return true;
        }
    }
    Microsoft::WRL::ComPtr<ID3DBlob> messages;
    const HRESULT result = D3DCompile(source, length, nullptr, nullptr, nullptr, "main", target, 0, 0,
                                      code.ReleaseAndGetAddressOf(), messages.GetAddressOf());
    if (SUCCEEDED(result)) {
        std::lock_guard lock(mutex);
        compiled.emplace(key, code);
        return true;
    }
    error = "shader did not compile";
    if (messages) {
        error += ": ";
        error.append(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
    }
    return false;
}

} // namespace veloplay::render
