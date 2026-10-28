#pragma once
// D3D9 fixed-function pipeline -> WGSL. A FixedFunctionKey captures everything a
// draw's shader depends on; GenerateWgsl() emits the matching vertex + fragment
// shader. Raster/output state (blend, depth, stencil, cull) is part of the key too
// so the same key also selects the wgpu::RenderPipeline.
#include <cstddef>
#include <cstdint>
#include <string>

namespace agpu::wgb {

// Vertex attribute slots used by the generated shaders (@location).
enum AttributeSlot : uint8_t {
  kSlotPosition = 0,
  kSlotNormal = 1,
  kSlotColor0 = 2,
  kSlotColor1 = 3,
  kSlotTexcoord0 = 4,  // ..7
  kSlotCount = 8,
};

// WGSL type class of an attribute as declared by the vertex format.
enum AttributeClass : uint8_t { kAttrNone = 0, kAttrFloat, kAttrSint, kAttrUint };

constexpr int kMaxStages = 4;
constexpr int kMaxTextureSlots = 4;  // stages / ps_1_x t registers that may sample a texture
constexpr int kMaxShaderInputs = 16;  // vs input registers v0..v15
constexpr int kMaxLights = 4;        // directional lights evaluated per vertex

#pragma pack(push, 1)
struct StageKey {
  uint8_t colorOp, colorArg0, colorArg1, colorArg2;
  uint8_t alphaOp, alphaArg0, alphaArg1, alphaArg2;
  uint8_t resultArg;
  uint8_t texCoordIndex;  // low bits of D3DTSS_TEXCOORDINDEX
  uint8_t texCoordGen;    // D3DTSS_TCI_* >> 16
  uint8_t ttfCount;       // D3DTTFF_COUNTn (0 = disabled)
  uint8_t ttfProjected;
  uint8_t hasTexture;
  uint8_t opaqueAlpha;    // X8R8G8B8 and friends: sampled alpha is 1
};

struct FixedFunctionKey {
  // Vertex input
  uint8_t attrClass[kSlotCount];
  uint8_t attrComponents[kSlotCount];
  uint8_t pretransformed;
  // Vertex processing
  uint8_t lighting, colorVertex, diffuseSource, ambientSource, specularSource, emissiveSource;
  uint8_t specularEnable, normalizeNormals, lightCount;
  uint8_t fogMode;  // 0 none, 1 linear, 2 exp, 3 exp2
  // Pixel processing
  uint8_t alphaTest, alphaFunc;
  StageKey stage[kMaxStages];
  uint8_t stageCount;
  // Programmable stages (shader_translator.h ids; 0 = fixed function)
  uint32_t vertexShader, pixelShader;
  uint8_t vsInputClass[kMaxShaderInputs];       // InputClass per vN
  uint8_t vsInputComponents[kMaxShaderInputs];
  uint8_t fogFactorFromShader;  // vertex fog with a vertex shader: use oFog
  uint8_t localViewer;          // D3DRS_LOCALVIEWER: per-vertex view vector for specular
};
#pragma pack(pop)

std::string GenerateWgsl(const FixedFunctionKey& key);

// Size of the per-draw uniform block (multiple of 256 for dynamic offsets).
constexpr size_t kUniformBlockSize = 1024;
// Vertex shader constants c0..c255 (separate dynamically offset binding).
constexpr size_t kVertexConstantsSize = 256 * 16;

// CPU-side layout of the uniform block; must match the WGSL struct U.
struct alignas(16) UniformBlock {
  float wvp[16];
  float wv[16];
  float world[16];
  float tex[kMaxTextureSlots][16];
  float viewport[4];  // x, y, width, height of the D3D viewport
  float matDiffuse[4], matAmbient[4], matSpecular[4], matEmissive[4];
  float globalAmbient[4];
  float fogColor[4];
  float fogParams[4];  // start, end, density, 1/(end-start)
  float tfactor[4];
  float misc[4];       // alphaRef (0..1), specular power, 0, 0
  float cameraPos[4];  // world space
  float cameraBack[4];  // world space, towards the camera along its axis (non-local viewer)
  float lightDir[kMaxLights][4];  // world space, pointing from the light
  float lightDiffuse[kMaxLights][4];
  float lightAmbient[kMaxLights][4];
  float lightSpecular[kMaxLights][4];
  float psConst[8][4];  // pixel shader c0..c7
};
static_assert(sizeof(UniformBlock) <= kUniformBlockSize);

}  // namespace agpu::wgb
