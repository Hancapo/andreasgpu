#pragma once
// D3D9 shader bytecode (vs_1_1..vs_2_x, ps_1_1..ps_1_4) -> WGSL.
//
// Vertex shaders become a WGSL vertex function writing the shared VOut struct
// (position, diffuse/specular = oD0/oD1, uv0..uv3 = oT0..oT3, fog); pixel shaders
// become fragment code producing `cur` from that struct. Vertex constants come from
// `vc` (array<vec4f, 256>), pixel constants from `u.psConst`.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace agpu::wgb {

struct ShaderInput {
  uint8_t reg;         // vN
  uint8_t usage;       // D3DDECLUSAGE_*
  uint8_t usageIndex;
};

struct ParsedShader {
  bool vertex = false;
  int major = 0, minor = 0;
  std::vector<DWORD> tokens;  // whole bytecode
  std::vector<ShaderInput> inputs;
  bool ok = false;
  std::string error;
};

// Registers bytecode and returns a stable non-zero id (identical code -> same id).
uint32_t RegisterShader(const DWORD* code, bool vertex);
const ParsedShader* GetShader(uint32_t id);

// WGSL type class of a vertex input as the vertex layout delivers it.
enum InputClass : uint8_t { kInputNone = 0, kInputFloat, kInputSint, kInputUint };

// `inputClass`/`inputComponents` are indexed by input register (v0..v15).
std::string TranslateVertexShader(const ParsedShader& shader, const uint8_t* inputClass,
                                  const uint8_t* inputComponents);
// Emits statements that leave the pixel shader result in `var cur: vec4f`.
std::string TranslatePixelShader(const ParsedShader& shader);

}  // namespace agpu::wgb
