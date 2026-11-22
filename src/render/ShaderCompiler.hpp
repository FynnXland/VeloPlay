#pragma once

#include <cstddef>
#include <string>

#include <d3dcommon.h>
#include <wrl/client.h>

namespace veloplay::render {

/// Compiles HLSL source with entry point "main" for `target` ("ps_4_0", "vs_4_0").
///
/// Compiled code does not belong to a device, so it is kept for the rest of the run: after a
/// lost graphics device everything is made anew, and compiling all shaders again kept the window
/// standing still for seconds. `source` has to stay where it is for the whole run (the shaders
/// are string constants), because its address is part of the key.
bool compileShader(const char* source, std::size_t length, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& code,
                   std::string& error);

} // namespace veloplay::render
