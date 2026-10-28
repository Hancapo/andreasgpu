#pragma once
#include <cstdint>

namespace agpu {

void InstallGameHooks();

// True while RenderWare's vertex shader generator is running; `desc` receives its
// 32-bit descriptor so CreateVertexShader can be attributed to it.
bool InVertexShaderGenerator(uint32_t* desc);

}  // namespace agpu
