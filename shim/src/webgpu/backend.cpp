// See backend.h. One Dawn device on D3D12, owned by the game's render thread.
//
// Data flow per frame:
//  - Static (managed-pool) vertex/index buffers live in GPU buffers updated on Unlock.
//  - Dynamic (default-pool) buffers are rewritten many times per frame, so each draw
//    copies the range it uses from a CPU shadow into a per-frame ring.
//  - Per-draw uniforms go into another per-frame ring (dynamic offsets).
//  - Both rings are uploaded with one WriteBuffer each before the frame is submitted,
//    so recorded draws always see the data as it was when they were issued.
#include "webgpu/backend.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "resources.h"
#include "webgpu/ffshader.h"
#include "webgpu/shader_translator.h"
#include "webgpu/mirror.h"

namespace agpu::wgb {
namespace {

// ---------------------------------------------------------------------------
// Types

// Stored in ResourceProxy::backend.
struct BackendObject {
  virtual ~BackendObject() = default;
};

uint32_t g_nextTextureId = 1;  // 0 = the white fallback

struct Texture : BackendObject {
  uint32_t id = g_nextTextureId++;
  wgpu::Texture texture;
  wgpu::TextureView view;
  D3DFORMAT d3dFormat = D3DFMT_UNKNOWN;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  UINT width = 0, height = 0, levels = 1;
  bool renderTarget = false;
  bool opaqueAlpha = false;
  bool convert = false;  // uploaded as BGRA8 after CPU conversion
  bool blockCompressed = false;
};

struct Buffer : BackendObject {
  wgpu::Buffer gpu;         // static buffers only
  std::vector<uint8_t> shadow;  // dynamic buffers and index buffers
  UINT length = 0;
  bool dynamic = false;
  bool index = false;
};

struct VertexElement {
  uint8_t stream;
  uint8_t slot;  // fixed-function attribute slot, 0xFF if none
  uint8_t usage, usageIndex;
  uint16_t offset;
  wgpu::VertexFormat format;
  AttributeClass cls;
  uint8_t components;
};

struct Decl {
  std::vector<VertexElement> elements;
  bool pretransformed = false;
  uint32_t streamMask = 0;
};

struct Light {
  D3DLIGHT9 light{};
  bool enabled = false;
};

struct State {
  DWORD rs[256] = {};
  DWORD tss[8][33] = {};
  DWORD samp[8][14] = {};
  ResourceProxy* textures[8] = {};
  D3DMATRIX world{}, view{}, projection{}, texture[8]{};
  std::map<DWORD, Light> lights;
  D3DMATERIAL9 material{};
  D3DVIEWPORT9 viewport{};
  struct Stream {
    ResourceProxy* buffer = nullptr;
    UINT offset = 0, stride = 0;
  } streams[4];
  ResourceProxy* indices = nullptr;
  const void* decl = nullptr;
  const void* vertexShader = nullptr;
  const void* pixelShader = nullptr;
  float vsConst[256][4] = {};
  float psConst[8][4] = {};
  ResourceProxy* target = nullptr;  // render-target texture; nullptr = back buffer
  bool depthBound = true;
};

#pragma pack(push, 1)
struct PipelineKey {
  FixedFunctionKey ff;
  // vertex layout
  struct Attr {
    uint8_t stream, slot, format;
    uint16_t offset;
  } attrs[kSlotCount];
  uint8_t attrCount;
  uint16_t strides[4];
  uint8_t topology, stripIndex;
  // output
  uint8_t blendEnable, srcBlend, dstBlend, blendOp, srcBlendA, dstBlendA, blendOpA, colorWrite;
  uint8_t hasDepth, zEnable, zWrite, zFunc, cull;
  uint8_t stencilEnable, twoSided, sFunc, sPass, sFail, sZFail, sFuncB, sPassB, sFailB, sZFailB;
  uint8_t stencilReadMask, stencilWriteMask;
  int32_t depthBias;
  float slopeBias;
};
#pragma pack(pop)

template <class T>
struct PodHash {
  size_t operator()(const T& k) const {
    auto p = reinterpret_cast<const uint8_t*>(&k);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(T); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
  }
};
template <class T>
struct PodEq {
  bool operator()(const T& a, const T& b) const { return std::memcmp(&a, &b, sizeof(T)) == 0; }
};

struct BindKey {
  uint32_t texture[kMaxTextureSlots];
  const void* sampler[kMaxTextureSlots];
};

// ---------------------------------------------------------------------------
// Globals (one device, game render thread only)

struct Backend {
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  bool active = false;

  // Mirror window
  HWND window = nullptr;
  wgpu::Surface surface;
  wgpu::SurfaceConfiguration surfaceConfig;
  bool surfaceConfigured = false;
  bool presentsInGameWindow = false;
  bool vsync = false;

  // Back buffer replacement
  UINT width = 0, height = 0;
  wgpu::Texture color, depth;
  wgpu::TextureView colorView, depthView;

  State state;
  std::unordered_map<const void*, Decl> decls;
  std::unordered_map<const void*, uint32_t> shaderIds;  // D3D shader object -> translator id

  wgpu::BindGroupLayout bindLayout;
  wgpu::PipelineLayout pipelineLayout;
  // Compiled asynchronously; a null pipeline means it is still compiling.
  std::unordered_map<PipelineKey, wgpu::RenderPipeline, PodHash<PipelineKey>, PodEq<PipelineKey>> pipelines;
  std::unordered_map<std::string, wgpu::ShaderModule> modules;
  std::unordered_map<uint64_t, wgpu::Sampler> samplers;
  std::unordered_map<BindKey, wgpu::BindGroup, PodHash<BindKey>, PodEq<BindKey>> bindGroups;
  wgpu::Texture white;
  wgpu::TextureView whiteView;

  // Per-frame rings
  std::vector<uint8_t> uniformData, vertexData;
  wgpu::Buffer uniformRing, vertexRing;
  size_t uniformCapacity = 0, vertexCapacity = 0;
  bool growUniform = false, growVertex = false;

  // Frame recording
  wgpu::CommandEncoder encoder;
  wgpu::RenderPassEncoder pass;
  bool passOpen = false;
  UINT passWidth = 0, passHeight = 0;
  bool passHasDepth = false;

  // Presentation
  wgpu::RenderPipeline blitPipeline;
  wgpu::BindGroupLayout blitLayout;
  wgpu::Sampler blitSampler;

  struct {
    unsigned draws = 0, drawn = 0, skippedShader = 0, skippedOther = 0, pipelinesCreated = 0, textureUploads = 0;
    unsigned waitingForPipeline = 0, slowFrames = 0;
    double worstFrameMs = 0, totalFrameMs = 0;
  } stats;
  unsigned frame = 0;
  LARGE_INTEGER lastPresent{};

  // Persistent pipeline cache (andreasgpu_cache/pipelines.bin).
  FILE* cacheFile = nullptr;
  std::set<uint32_t> cachedShaders;
  size_t warmupCount = 0;
  LARGE_INTEGER warmupStart{};
  bool warmupDone = true;

  // Frame readback for the comparison harness (compare.h).
  bool captureRequested = false;
  std::vector<uint8_t> capture;  // BGRA8, tightly packed
  UINT captureWidth = 0, captureHeight = 0;
};
Backend* g = nullptr;

std::string Str(wgpu::StringView s) { return std::string(s.data ? s.data : "", s.data ? s.length : 0); }

float AsFloat(DWORD v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void ColorToFloats(D3DCOLOR c, float* out) {
  out[0] = ((c >> 16) & 0xFF) / 255.0f;
  out[1] = ((c >> 8) & 0xFF) / 255.0f;
  out[2] = (c & 0xFF) / 255.0f;
  out[3] = ((c >> 24) & 0xFF) / 255.0f;
}

void ValueToFloats(const D3DCOLORVALUE& c, float* out) {
  out[0] = c.r;
  out[1] = c.g;
  out[2] = c.b;
  out[3] = c.a;
}

D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b) {
  D3DMATRIX r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  return r;
}

D3DMATRIX Identity() {
  D3DMATRIX m{};
  m._11 = m._22 = m._33 = m._44 = 1.0f;
  return m;
}

size_t Align(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

Texture* TextureOf(ResourceProxy* p) {
  return p && p->backend ? dynamic_cast<Texture*>(static_cast<BackendObject*>(p->backend)) : nullptr;
}
Buffer* BufferOf(ResourceProxy* p) {
  return p && p->backend ? dynamic_cast<Buffer*>(static_cast<BackendObject*>(p->backend)) : nullptr;
}

// ---------------------------------------------------------------------------
// Defaults (D3D9 documentation)

void ResetState(BOOL autoDepth) {
  State& s = g->state;
  s = State{};
  DWORD* rs = s.rs;
  rs[D3DRS_ZENABLE] = autoDepth ? D3DZB_TRUE : D3DZB_FALSE;
  rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  rs[D3DRS_ZWRITEENABLE] = TRUE;
  rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_FOGEND] = 0x3F800000;
  rs[D3DRS_FOGDENSITY] = 0x3F800000;
  rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
  rs[D3DRS_LIGHTING] = TRUE;
  rs[D3DRS_COLORVERTEX] = TRUE;
  rs[D3DRS_LOCALVIEWER] = TRUE;
  rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  rs[D3DRS_COLORWRITEENABLE] = 0xF;
  rs[D3DRS_BLENDOP] = rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
  for (DWORD i = 0; i < 8; ++i) {
    DWORD* t = s.tss[i];
    t[D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    t[D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    t[D3DTSS_COLORARG2] = t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    t[D3DTSS_COLORARG0] = t[D3DTSS_ALPHAARG0] = t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
    t[D3DTSS_TEXCOORDINDEX] = i;
    DWORD* sm = s.samp[i];
    sm[D3DSAMP_ADDRESSU] = sm[D3DSAMP_ADDRESSV] = sm[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    sm[D3DSAMP_MAGFILTER] = sm[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    sm[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    sm[D3DSAMP_MAXANISOTROPY] = 1;
    s.texture[i] = Identity();
  }
  s.world = s.view = s.projection = Identity();
  s.viewport = {0, 0, g->width, g->height, 0.0f, 1.0f};
  s.depthBound = autoDepth != FALSE;
}

// ---------------------------------------------------------------------------
// Device setup

wgpu::ShaderModule ModuleFromWgsl(const std::string& code) {
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor desc{};
  desc.nextInChain = &wgsl;
  return g->device.CreateShaderModule(&desc);
}

void CreateTargets(UINT width, UINT height) {
  if (g->width == width && g->height == height && g->color) return;
  g->width = width;
  g->height = height;
  wgpu::TextureDescriptor desc{};
  desc.size = {width, height, 1};
  desc.format = wgpu::TextureFormat::BGRA8Unorm;
  desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
  g->color = g->device.CreateTexture(&desc);
  g->colorView = g->color.CreateView();
  desc.format = wgpu::TextureFormat::Depth24PlusStencil8;
  desc.usage = wgpu::TextureUsage::RenderAttachment;
  g->depth = g->device.CreateTexture(&desc);
  g->depthView = g->depth.CreateView();
}

void CreateFixedObjects() {
  constexpr int kEntries = 2 + 2 * kMaxTextureSlots;
  wgpu::BindGroupLayoutEntry entries[kEntries] = {};
  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[0].buffer.hasDynamicOffset = true;
  entries[0].buffer.minBindingSize = sizeof(UniformBlock);
  for (int i = 0; i < kMaxTextureSlots; ++i) {
    entries[1 + 2 * i].binding = 1 + 2 * i;
    entries[1 + 2 * i].visibility = wgpu::ShaderStage::Fragment;
    entries[1 + 2 * i].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[1 + 2 * i].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[2 + 2 * i].binding = 2 + 2 * i;
    entries[2 + 2 * i].visibility = wgpu::ShaderStage::Fragment;
    entries[2 + 2 * i].sampler.type = wgpu::SamplerBindingType::Filtering;
  }
  entries[kEntries - 1].binding = 1 + 2 * kMaxTextureSlots;
  entries[kEntries - 1].visibility = wgpu::ShaderStage::Vertex;
  entries[kEntries - 1].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[kEntries - 1].buffer.hasDynamicOffset = true;
  entries[kEntries - 1].buffer.minBindingSize = kVertexConstantsSize;
  wgpu::BindGroupLayoutDescriptor layoutDesc{};
  layoutDesc.entryCount = kEntries;
  layoutDesc.entries = entries;
  g->bindLayout = g->device.CreateBindGroupLayout(&layoutDesc);
  wgpu::PipelineLayoutDescriptor pipelineLayoutDesc{};
  pipelineLayoutDesc.bindGroupLayoutCount = 1;
  pipelineLayoutDesc.bindGroupLayouts = &g->bindLayout;
  g->pipelineLayout = g->device.CreatePipelineLayout(&pipelineLayoutDesc);

  wgpu::TextureDescriptor whiteDesc{};
  whiteDesc.size = {1, 1, 1};
  whiteDesc.format = wgpu::TextureFormat::BGRA8Unorm;
  whiteDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  g->white = g->device.CreateTexture(&whiteDesc);
  g->whiteView = g->white.CreateView();
  uint32_t pixel = 0xFFFFFFFF;
  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = g->white;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = 4;
  wgpu::Extent3D one{1, 1, 1};
  g->queue.WriteTexture(&dst, &pixel, 4, &layout, &one);

  // Blit: back buffer -> mirror window, scaled.
  const char* blit = R"(
@group(0) @binding(0) var t: texture_2d<f32>;
@group(0) @binding(1) var s: sampler;
struct V { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> V {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return V(vec4f(p * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0), p);
}
@fragment fn fs(v: V) -> @location(0) vec4f { return vec4f(textureSample(t, s, v.uv).rgb, 1.0); }
)";
  wgpu::ShaderModule module = ModuleFromWgsl(blit);
  wgpu::BindGroupLayoutEntry blitEntries[2] = {};
  blitEntries[0].binding = 0;
  blitEntries[0].visibility = wgpu::ShaderStage::Fragment;
  blitEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  blitEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  blitEntries[1].binding = 1;
  blitEntries[1].visibility = wgpu::ShaderStage::Fragment;
  blitEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
  wgpu::BindGroupLayoutDescriptor blitLayoutDesc{};
  blitLayoutDesc.entryCount = 2;
  blitLayoutDesc.entries = blitEntries;
  g->blitLayout = g->device.CreateBindGroupLayout(&blitLayoutDesc);
  wgpu::PipelineLayoutDescriptor blitPipelineLayoutDesc{};
  blitPipelineLayoutDesc.bindGroupLayoutCount = 1;
  blitPipelineLayoutDesc.bindGroupLayouts = &g->blitLayout;
  wgpu::ColorTargetState target{};
  target.format = g->surfaceConfig.format;
  wgpu::FragmentState fragment{};
  fragment.module = module;
  fragment.entryPoint = "fs";
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor desc{};
  desc.layout = g->device.CreatePipelineLayout(&blitPipelineLayoutDesc);
  desc.vertex.module = module;
  desc.vertex.entryPoint = "vs";
  desc.fragment = &fragment;
  g->blitPipeline = g->device.CreateRenderPipeline(&desc);
  wgpu::SamplerDescriptor samplerDesc{};
  samplerDesc.magFilter = samplerDesc.minFilter = wgpu::FilterMode::Linear;
  g->blitSampler = g->device.CreateSampler(&samplerDesc);
}

void EnsureRings() {
  auto make = [](size_t size) {
    wgpu::BufferDescriptor desc{};
    desc.size = size;
    desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform | wgpu::BufferUsage::Vertex |
                 wgpu::BufferUsage::Index;
    return g->device.CreateBuffer(&desc);
  };
  if (!g->uniformRing || g->growUniform) {
    g->uniformCapacity = g->uniformCapacity ? g->uniformCapacity * 2 : (4u << 20);
    g->uniformRing = make(g->uniformCapacity);
    g->bindGroups.clear();  // they reference the ring
    g->growUniform = false;
  }
  if (!g->vertexRing || g->growVertex) {
    g->vertexCapacity = g->vertexCapacity ? g->vertexCapacity * 2 : (32u << 20);
    g->vertexRing = make(g->vertexCapacity);
    g->growVertex = false;
  }
}

// ---------------------------------------------------------------------------
// Formats

struct VertexFormatInfo {
  wgpu::VertexFormat format;
  AttributeClass cls;
  uint8_t components;
};

bool MapDeclType(BYTE type, VertexFormatInfo* out) {
  switch (type) {
    case D3DDECLTYPE_FLOAT1: *out = {wgpu::VertexFormat::Float32, kAttrFloat, 1}; return true;
    case D3DDECLTYPE_FLOAT2: *out = {wgpu::VertexFormat::Float32x2, kAttrFloat, 2}; return true;
    case D3DDECLTYPE_FLOAT3: *out = {wgpu::VertexFormat::Float32x3, kAttrFloat, 3}; return true;
    case D3DDECLTYPE_FLOAT4: *out = {wgpu::VertexFormat::Float32x4, kAttrFloat, 4}; return true;
    case D3DDECLTYPE_D3DCOLOR: *out = {wgpu::VertexFormat::Unorm8x4BGRA, kAttrFloat, 4}; return true;
    case D3DDECLTYPE_UBYTE4: *out = {wgpu::VertexFormat::Uint8x4, kAttrUint, 4}; return true;
    case D3DDECLTYPE_SHORT2: *out = {wgpu::VertexFormat::Sint16x2, kAttrSint, 2}; return true;
    case D3DDECLTYPE_SHORT4: *out = {wgpu::VertexFormat::Sint16x4, kAttrSint, 4}; return true;
    case D3DDECLTYPE_UBYTE4N: *out = {wgpu::VertexFormat::Unorm8x4, kAttrFloat, 4}; return true;
    case D3DDECLTYPE_SHORT2N: *out = {wgpu::VertexFormat::Snorm16x2, kAttrFloat, 2}; return true;
    case D3DDECLTYPE_SHORT4N: *out = {wgpu::VertexFormat::Snorm16x4, kAttrFloat, 4}; return true;
    case D3DDECLTYPE_USHORT2N: *out = {wgpu::VertexFormat::Unorm16x2, kAttrFloat, 2}; return true;
    case D3DDECLTYPE_USHORT4N: *out = {wgpu::VertexFormat::Unorm16x4, kAttrFloat, 4}; return true;
    case D3DDECLTYPE_FLOAT16_2: *out = {wgpu::VertexFormat::Float16x2, kAttrFloat, 2}; return true;
    case D3DDECLTYPE_FLOAT16_4: *out = {wgpu::VertexFormat::Float16x4, kAttrFloat, 4}; return true;
    default: return false;
  }
}

int SlotForUsage(BYTE usage, BYTE index) {
  switch (usage) {
    case D3DDECLUSAGE_POSITION:
    case D3DDECLUSAGE_POSITIONT: return index == 0 ? kSlotPosition : -1;
    case D3DDECLUSAGE_NORMAL: return index == 0 ? kSlotNormal : -1;
    case D3DDECLUSAGE_COLOR: return index < 2 ? kSlotColor0 + index : -1;
    case D3DDECLUSAGE_TEXCOORD: return index < 4 ? kSlotTexcoord0 + index : -1;
    default: return -1;
  }
}

// Returns false for formats that are not translated (texture stays white).
bool MapTextureFormat(D3DFORMAT f, Texture* t) {
  t->format = wgpu::TextureFormat::BGRA8Unorm;
  switch (f) {
    case D3DFMT_A8R8G8B8: return true;
    case D3DFMT_X8R8G8B8: t->opaqueAlpha = true; return true;
    case MAKEFOURCC('D', 'X', 'T', '1'):
      t->format = wgpu::TextureFormat::BC1RGBAUnorm;
      t->blockCompressed = true;
      return true;
    case MAKEFOURCC('D', 'X', 'T', '2'):
    case MAKEFOURCC('D', 'X', 'T', '3'):
      t->format = wgpu::TextureFormat::BC2RGBAUnorm;
      t->blockCompressed = true;
      return true;
    case MAKEFOURCC('D', 'X', 'T', '4'):
    case MAKEFOURCC('D', 'X', 'T', '5'):
      t->format = wgpu::TextureFormat::BC3RGBAUnorm;
      t->blockCompressed = true;
      return true;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_R8G8B8:
    case D3DFMT_L8:
    case D3DFMT_A8:
    case D3DFMT_A8L8: t->convert = true; return true;
    default: return false;
  }
}

uint32_t ConvertTexel(D3DFORMAT f, const uint8_t* p) {
  auto expand = [](uint32_t v, int bits) { return (v * 255 + ((1 << bits) - 1) / 2) / ((1 << bits) - 1); };
  uint32_t a = 255, r, gr, b;
  switch (f) {
    case D3DFMT_R5G6B5: {
      uint16_t v = p[0] | (p[1] << 8);
      r = expand(v >> 11, 5), gr = expand((v >> 5) & 63, 6), b = expand(v & 31, 5);
      break;
    }
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: {
      uint16_t v = p[0] | (p[1] << 8);
      r = expand((v >> 10) & 31, 5), gr = expand((v >> 5) & 31, 5), b = expand(v & 31, 5);
      if (f == D3DFMT_A1R5G5B5) a = (v & 0x8000) ? 255 : 0;
      break;
    }
    case D3DFMT_A4R4G4B4: {
      uint16_t v = p[0] | (p[1] << 8);
      a = expand(v >> 12, 4), r = expand((v >> 8) & 15, 4), gr = expand((v >> 4) & 15, 4), b = expand(v & 15, 4);
      break;
    }
    case D3DFMT_R8G8B8: b = p[0], gr = p[1], r = p[2]; break;
    case D3DFMT_L8: r = gr = b = p[0]; break;
    case D3DFMT_A8: r = gr = b = 255, a = p[0]; break;
    case D3DFMT_A8L8: r = gr = b = p[0], a = p[1]; break;
    default: r = gr = b = 255; break;
  }
  return (a << 24) | (r << 16) | (gr << 8) | b;
}

UINT BytesPerTexel(D3DFORMAT f) {
  switch (f) {
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_L8:
    case D3DFMT_A8: return 1;
    default: return 2;
  }
}

// ---------------------------------------------------------------------------
// Passes

wgpu::TextureView TargetView(UINT* width, UINT* height) {
  Texture* t = TextureOf(g->state.target);
  if (t && t->renderTarget) {
    *width = t->width;
    *height = t->height;
    return t->view;
  }
  *width = g->width;
  *height = g->height;
  return g->colorView;
}

void EndPass() {
  if (!g->passOpen) return;
  g->pass.End();
  g->pass = nullptr;
  g->passOpen = false;
}

void EnsureEncoder() {
  if (!g->encoder) g->encoder = g->device.CreateCommandEncoder();
}

void BeginPass(DWORD clearFlags, D3DCOLOR color, float z, DWORD stencil) {
  EndPass();
  EnsureEncoder();
  UINT width, height;
  wgpu::RenderPassColorAttachment colorAttachment{};
  colorAttachment.view = TargetView(&width, &height);
  colorAttachment.loadOp = (clearFlags & D3DCLEAR_TARGET) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
  colorAttachment.storeOp = wgpu::StoreOp::Store;
  float c[4];
  ColorToFloats(color, c);
  colorAttachment.clearValue = {c[0], c[1], c[2], c[3]};
  wgpu::RenderPassDepthStencilAttachment depthAttachment{};
  // The auto depth buffer only fits targets of the back-buffer size.
  bool useDepth = g->state.depthBound && width == g->width && height == g->height;
  if (useDepth) {
    depthAttachment.view = g->depthView;
    depthAttachment.depthLoadOp = (clearFlags & D3DCLEAR_ZBUFFER) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depthAttachment.depthStoreOp = wgpu::StoreOp::Store;
    depthAttachment.depthClearValue = std::clamp(z, 0.0f, 1.0f);
    depthAttachment.stencilLoadOp = (clearFlags & D3DCLEAR_STENCIL) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depthAttachment.stencilStoreOp = wgpu::StoreOp::Store;
    depthAttachment.stencilClearValue = stencil & 0xFF;
  }
  wgpu::RenderPassDescriptor desc{};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &colorAttachment;
  desc.depthStencilAttachment = useDepth ? &depthAttachment : nullptr;
  g->pass = g->encoder.BeginRenderPass(&desc);
  g->passOpen = true;
  g->passWidth = width;
  g->passHeight = height;
  g->passHasDepth = useDepth;
}

void EnsurePass() {
  if (!g->passOpen) BeginPass(0, 0, 1.0f, 0);
}

// ---------------------------------------------------------------------------
// Pipelines

wgpu::BlendFactor MapBlend(DWORD b, bool alpha) {
  switch (b) {
    case D3DBLEND_ZERO: return wgpu::BlendFactor::Zero;
    case D3DBLEND_ONE: return wgpu::BlendFactor::One;
    case D3DBLEND_SRCCOLOR: return alpha ? wgpu::BlendFactor::SrcAlpha : wgpu::BlendFactor::Src;
    case D3DBLEND_INVSRCCOLOR: return alpha ? wgpu::BlendFactor::OneMinusSrcAlpha : wgpu::BlendFactor::OneMinusSrc;
    case D3DBLEND_SRCALPHA: return wgpu::BlendFactor::SrcAlpha;
    case D3DBLEND_INVSRCALPHA: return wgpu::BlendFactor::OneMinusSrcAlpha;
    case D3DBLEND_DESTALPHA: return wgpu::BlendFactor::DstAlpha;
    case D3DBLEND_INVDESTALPHA: return wgpu::BlendFactor::OneMinusDstAlpha;
    case D3DBLEND_DESTCOLOR: return alpha ? wgpu::BlendFactor::DstAlpha : wgpu::BlendFactor::Dst;
    case D3DBLEND_INVDESTCOLOR: return alpha ? wgpu::BlendFactor::OneMinusDstAlpha : wgpu::BlendFactor::OneMinusDst;
    case D3DBLEND_SRCALPHASAT: return wgpu::BlendFactor::SrcAlphaSaturated;
    case D3DBLEND_BLENDFACTOR: return wgpu::BlendFactor::Constant;
    case D3DBLEND_INVBLENDFACTOR: return wgpu::BlendFactor::OneMinusConstant;
    default: return wgpu::BlendFactor::One;
  }
}

wgpu::BlendOperation MapBlendOp(DWORD op) {
  switch (op) {
    case D3DBLENDOP_SUBTRACT: return wgpu::BlendOperation::Subtract;
    case D3DBLENDOP_REVSUBTRACT: return wgpu::BlendOperation::ReverseSubtract;
    case D3DBLENDOP_MIN: return wgpu::BlendOperation::Min;
    case D3DBLENDOP_MAX: return wgpu::BlendOperation::Max;
    default: return wgpu::BlendOperation::Add;
  }
}

wgpu::CompareFunction MapCompare(DWORD c) {
  switch (c) {
    case D3DCMP_NEVER: return wgpu::CompareFunction::Never;
    case D3DCMP_LESS: return wgpu::CompareFunction::Less;
    case D3DCMP_EQUAL: return wgpu::CompareFunction::Equal;
    case D3DCMP_LESSEQUAL: return wgpu::CompareFunction::LessEqual;
    case D3DCMP_GREATER: return wgpu::CompareFunction::Greater;
    case D3DCMP_NOTEQUAL: return wgpu::CompareFunction::NotEqual;
    case D3DCMP_GREATEREQUAL: return wgpu::CompareFunction::GreaterEqual;
    default: return wgpu::CompareFunction::Always;
  }
}

wgpu::StencilOperation MapStencilOp(DWORD op) {
  switch (op) {
    case D3DSTENCILOP_ZERO: return wgpu::StencilOperation::Zero;
    case D3DSTENCILOP_REPLACE: return wgpu::StencilOperation::Replace;
    case D3DSTENCILOP_INCRSAT: return wgpu::StencilOperation::IncrementClamp;
    case D3DSTENCILOP_DECRSAT: return wgpu::StencilOperation::DecrementClamp;
    case D3DSTENCILOP_INVERT: return wgpu::StencilOperation::Invert;
    case D3DSTENCILOP_INCR: return wgpu::StencilOperation::IncrementWrap;
    case D3DSTENCILOP_DECR: return wgpu::StencilOperation::DecrementWrap;
    default: return wgpu::StencilOperation::Keep;
  }
}

// ---------------------------------------------------------------------------
// Pipeline cache. Records hold D3D state (pipeline keys), not generated code, so a
// better WGSL generator reuses old caches. Shader ids differ per run, so the
// bytecode of every referenced shader is stored and re-registered on load.

constexpr char kCacheMagic[8] = {'A', 'G', 'P', 'U', 'P', 'C', '1', 0};
constexpr uint32_t kCacheVersion = 1;
enum : uint8_t { kRecordShader = 1, kRecordPipeline = 2 };

void CacheWriteShader(uint32_t id) {
  if (!id || g->cachedShaders.count(id)) return;
  const ParsedShader* shader = GetShader(id);
  if (!shader) return;
  uint8_t type = kRecordShader, vertex = shader->vertex;
  uint32_t count = static_cast<uint32_t>(shader->tokens.size());
  fwrite(&type, 1, 1, g->cacheFile);
  fwrite(&id, 4, 1, g->cacheFile);
  fwrite(&vertex, 1, 1, g->cacheFile);
  fwrite(&count, 4, 1, g->cacheFile);
  fwrite(shader->tokens.data(), 4, count, g->cacheFile);
  g->cachedShaders.insert(id);
}

void CacheWritePipeline(const PipelineKey& key) {
  if (!g->cacheFile) return;
  CacheWriteShader(key.ff.vertexShader);
  CacheWriteShader(key.ff.pixelShader);
  uint8_t type = kRecordPipeline;
  fwrite(&type, 1, 1, g->cacheFile);
  fwrite(&key, sizeof(key), 1, g->cacheFile);
  fflush(g->cacheFile);  // survives a crash
}

std::vector<PipelineKey> ReadPipelineCache(const std::wstring& path) {
  std::vector<PipelineKey> keys;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return keys;
  char magic[8];
  uint32_t version = 0, keySize = 0;
  bool ok = fread(magic, 8, 1, f) == 1 && fread(&version, 4, 1, f) == 1 && fread(&keySize, 4, 1, f) == 1 &&
            std::memcmp(magic, kCacheMagic, 8) == 0 && version == kCacheVersion && keySize == sizeof(PipelineKey);
  std::unordered_map<uint32_t, uint32_t> remap;  // id in the file -> id in this run
  uint8_t type;
  while (ok && fread(&type, 1, 1, f) == 1) {
    if (type == kRecordShader) {
      uint32_t id, count;
      uint8_t vertex;
      if (fread(&id, 4, 1, f) != 1 || fread(&vertex, 1, 1, f) != 1 || fread(&count, 4, 1, f) != 1 || count > 65536)
        break;
      std::vector<DWORD> tokens(count);
      if (fread(tokens.data(), 4, count, f) != count || count < 2 || tokens.back() != 0x0000FFFF) break;
      remap[id] = RegisterShader(tokens.data(), vertex != 0);
    } else if (type == kRecordPipeline) {
      PipelineKey key;
      if (fread(&key, sizeof(key), 1, f) != 1) break;
      auto fix = [&](uint32_t& id) {
        if (!id) return true;
        auto it = remap.find(id);
        if (it == remap.end()) return false;
        id = it->second;
        return true;
      };
      if (fix(key.ff.vertexShader) && fix(key.ff.pixelShader)) keys.push_back(key);
    } else {
      break;  // unknown record: keep what was read so far
    }
  }
  fclose(f);
  if (!ok) Logf("[webgpu] pipeline cache %ls ignored (different format)\n", path.c_str());
  return keys;
}

wgpu::RenderPipeline GetPipeline(const PipelineKey& key);

// Compiles every pipeline seen in earlier sessions, in the background, while the
// game is still starting up; then keeps recording new ones.
void WarmUpPipelineCache() {
  std::wstring dir = ModuleRelativePath(L"andreasgpu_cache");
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = dir + L"\\pipelines.bin";
  std::vector<PipelineKey> keys = ReadPipelineCache(path);
  // Rewrite the file with this run's shader ids; new pipelines are appended.
  g->cacheFile = _wfopen(path.c_str(), L"wb");
  if (g->cacheFile) {
    uint32_t version = kCacheVersion, keySize = sizeof(PipelineKey);
    fwrite(kCacheMagic, 8, 1, g->cacheFile);
    fwrite(&version, 4, 1, g->cacheFile);
    fwrite(&keySize, 4, 1, g->cacheFile);
  }
  QueryPerformanceCounter(&g->warmupStart);
  for (const PipelineKey& key : keys) GetPipeline(key);  // skips duplicates, records, compiles async
  g->warmupCount = g->pipelines.size();
  g->warmupDone = g->warmupCount == 0;
  Logf("[webgpu] pipeline cache: compiling %zu pipelines from earlier sessions\n", g->warmupCount);
  LogFlush();
}

wgpu::RenderPipeline GetPipeline(const PipelineKey& key) {
  auto it = g->pipelines.find(key);
  if (it != g->pipelines.end()) return it->second;  // null while compiling

  std::string wgsl = GenerateWgsl(key.ff);
  auto mit = g->modules.find(wgsl);
  if (mit == g->modules.end() && (key.ff.vertexShader || key.ff.pixelShader)) {
    // Translated shaders: keep the WGSL for debugging (andreasgpu_logs/wgsl_vsN_psM_K.wgsl).
    wchar_t name[64];
    swprintf(name, 64, L"wgsl_vs%u_ps%u_%zu.wgsl", key.ff.vertexShader, key.ff.pixelShader, g->modules.size());
    WriteLogFile(name, wgsl.data(), wgsl.size());
  }
  wgpu::ShaderModule module = mit != g->modules.end() ? mit->second : (g->modules[wgsl] = ModuleFromWgsl(wgsl));

  wgpu::VertexAttribute attributes[4][kSlotCount];
  wgpu::VertexBufferLayout buffers[4];
  uint32_t bufferCount = 0;
  for (int stream = 0; stream < 4; ++stream) {
    uint32_t count = 0;
    for (int i = 0; i < key.attrCount; ++i) {
      const auto& a = key.attrs[i];
      if (a.stream != stream) continue;
      attributes[stream][count].format = static_cast<wgpu::VertexFormat>(a.format);
      attributes[stream][count].offset = a.offset;
      attributes[stream][count].shaderLocation = a.slot;
      ++count;
    }
    if (!count) continue;
    bufferCount = stream + 1;
    buffers[stream].arrayStride = key.strides[stream];
    buffers[stream].attributeCount = count;
    buffers[stream].attributes = attributes[stream];
  }
  for (uint32_t i = 0; i < bufferCount; ++i)
    if (!buffers[i].attributeCount) buffers[i].stepMode = wgpu::VertexStepMode::Undefined;

  wgpu::BlendState blend{};
  blend.color = {MapBlendOp(key.blendOp), MapBlend(key.srcBlend, false), MapBlend(key.dstBlend, false)};
  blend.alpha = {MapBlendOp(key.blendOpA), MapBlend(key.srcBlendA, true), MapBlend(key.dstBlendA, true)};
  if (key.blendOp == D3DBLENDOP_MIN || key.blendOp == D3DBLENDOP_MAX)
    blend.color.srcFactor = blend.color.dstFactor = wgpu::BlendFactor::One;
  if (key.blendOpA == D3DBLENDOP_MIN || key.blendOpA == D3DBLENDOP_MAX)
    blend.alpha.srcFactor = blend.alpha.dstFactor = wgpu::BlendFactor::One;
  wgpu::ColorTargetState target{};
  target.format = wgpu::TextureFormat::BGRA8Unorm;
  target.blend = key.blendEnable ? &blend : nullptr;
  target.writeMask = static_cast<wgpu::ColorWriteMask>(key.colorWrite & 0xF);
  wgpu::FragmentState fragment{};
  fragment.module = module;
  fragment.entryPoint = "fs";
  fragment.targetCount = 1;
  fragment.targets = &target;

  wgpu::DepthStencilState depthStencil{};
  if (key.hasDepth) {
    depthStencil.format = wgpu::TextureFormat::Depth24PlusStencil8;
    depthStencil.depthWriteEnabled = key.zEnable && key.zWrite ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
    depthStencil.depthCompare = key.zEnable ? MapCompare(key.zFunc) : wgpu::CompareFunction::Always;
    if (key.stencilEnable) {
      depthStencil.stencilFront = {MapCompare(key.sFunc), MapStencilOp(key.sFail), MapStencilOp(key.sZFail),
                                   MapStencilOp(key.sPass)};
      depthStencil.stencilBack = key.twoSided ? wgpu::StencilFaceState{MapCompare(key.sFuncB), MapStencilOp(key.sFailB),
                                                                        MapStencilOp(key.sZFailB),
                                                                        MapStencilOp(key.sPassB)}
                                              : depthStencil.stencilFront;
      depthStencil.stencilReadMask = key.stencilReadMask;
      depthStencil.stencilWriteMask = key.stencilWriteMask;
    } else {
      depthStencil.stencilReadMask = depthStencil.stencilWriteMask = 0;
    }
    depthStencil.depthBias = key.depthBias;
    depthStencil.depthBiasSlopeScale = key.slopeBias;
  }

  wgpu::RenderPipelineDescriptor desc{};
  desc.layout = g->pipelineLayout;
  desc.vertex.module = module;
  desc.vertex.entryPoint = "vs";
  desc.vertex.bufferCount = bufferCount;
  desc.vertex.buffers = buffers;
  desc.primitive.topology = static_cast<wgpu::PrimitiveTopology>(key.topology);
  desc.primitive.stripIndexFormat = static_cast<wgpu::IndexFormat>(key.stripIndex);
  desc.primitive.frontFace = wgpu::FrontFace::CW;  // D3D's front faces are clockwise
  desc.primitive.cullMode = key.cull == D3DCULL_CCW  ? wgpu::CullMode::Back
                            : key.cull == D3DCULL_CW ? wgpu::CullMode::Front
                                                     : wgpu::CullMode::None;
  desc.depthStencil = key.hasDepth ? &depthStencil : nullptr;
  desc.fragment = &fragment;
  // Compiling (WGSL -> HLSL -> DXC -> PSO) takes tens of milliseconds; doing it on the
  // game thread mid-frame is what stuttered. The draw is skipped until it is ready.
  ++g->stats.pipelinesCreated;
  g->pipelines[key] = nullptr;
  CacheWritePipeline(key);
  g->device.CreateRenderPipelineAsync(
      &desc, wgpu::CallbackMode::AllowProcessEvents,
      [key](wgpu::CreatePipelineAsyncStatus status, wgpu::RenderPipeline pipeline, wgpu::StringView message) {
        if (!g) return;
        if (status == wgpu::CreatePipelineAsyncStatus::Success)
          g->pipelines[key] = std::move(pipeline);
        else
          Logf("[webgpu] pipeline creation failed: %s\n", Str(message).c_str());
      });
  return nullptr;
}

wgpu::Sampler GetSampler(DWORD stage) {
  const DWORD* s = g->state.samp[stage];
  auto address = [](DWORD a) {
    switch (a) {
      case D3DTADDRESS_MIRROR: return wgpu::AddressMode::MirrorRepeat;
      case D3DTADDRESS_CLAMP:
      case D3DTADDRESS_BORDER: return wgpu::AddressMode::ClampToEdge;
      default: return wgpu::AddressMode::Repeat;
    }
  };
  bool aniso = s[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || s[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC;
  uint16_t maxAniso = aniso ? static_cast<uint16_t>(std::clamp<DWORD>(s[D3DSAMP_MAXANISOTROPY], 1, 16)) : 1;
  uint64_t key = (uint64_t)s[D3DSAMP_ADDRESSU] | ((uint64_t)s[D3DSAMP_ADDRESSV] << 4) |
                 ((uint64_t)s[D3DSAMP_MAGFILTER] << 8) | ((uint64_t)s[D3DSAMP_MINFILTER] << 12) |
                 ((uint64_t)s[D3DSAMP_MIPFILTER] << 16) | ((uint64_t)maxAniso << 20);
  auto it = g->samplers.find(key);
  if (it != g->samplers.end()) return it->second;
  wgpu::SamplerDescriptor desc{};
  desc.addressModeU = address(s[D3DSAMP_ADDRESSU]);
  desc.addressModeV = address(s[D3DSAMP_ADDRESSV]);
  desc.addressModeW = address(s[D3DSAMP_ADDRESSW]);
  desc.magFilter = s[D3DSAMP_MAGFILTER] == D3DTEXF_POINT ? wgpu::FilterMode::Nearest : wgpu::FilterMode::Linear;
  desc.minFilter = s[D3DSAMP_MINFILTER] == D3DTEXF_POINT ? wgpu::FilterMode::Nearest : wgpu::FilterMode::Linear;
  desc.mipmapFilter =
      s[D3DSAMP_MIPFILTER] == D3DTEXF_LINEAR ? wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest;
  if (s[D3DSAMP_MIPFILTER] == D3DTEXF_NONE) desc.lodMaxClamp = 0.0f;
  if (maxAniso > 1) {
    desc.magFilter = desc.minFilter = wgpu::FilterMode::Linear;
    desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  }
  desc.maxAnisotropy = maxAniso;
  return g->samplers[key] = g->device.CreateSampler(&desc);
}

wgpu::BindGroup GetBindGroup(const FixedFunctionKey& ff) {
  BindKey key{};
  wgpu::TextureView views[kMaxTextureSlots];
  wgpu::Sampler samplers[kMaxTextureSlots];
  for (int i = 0; i < kMaxTextureSlots; ++i) {
    Texture* t = ff.stage[i].hasTexture ? TextureOf(g->state.textures[i]) : nullptr;
    views[i] = t && t->view ? t->view : g->whiteView;
    samplers[i] = GetSampler(i);
    key.texture[i] = t && t->view ? t->id : 0;
    key.sampler[i] = samplers[i].Get();
  }
  auto it = g->bindGroups.find(key);
  if (it != g->bindGroups.end()) return it->second;
  constexpr int kEntries = 2 + 2 * kMaxTextureSlots;
  wgpu::BindGroupEntry entries[kEntries] = {};
  entries[0].binding = 0;
  entries[0].buffer = g->uniformRing;
  entries[0].size = sizeof(UniformBlock);
  entries[kEntries - 1].binding = 1 + 2 * kMaxTextureSlots;
  entries[kEntries - 1].buffer = g->uniformRing;
  entries[kEntries - 1].size = kVertexConstantsSize;
  for (int i = 0; i < kMaxTextureSlots; ++i) {
    entries[1 + 2 * i].binding = 1 + 2 * i;
    entries[1 + 2 * i].textureView = views[i];
    entries[2 + 2 * i].binding = 2 + 2 * i;
    entries[2 + 2 * i].sampler = samplers[i];
  }
  wgpu::BindGroupDescriptor desc{};
  desc.layout = g->bindLayout;
  desc.entryCount = kEntries;
  desc.entries = entries;
  return g->bindGroups[key] = g->device.CreateBindGroup(&desc);
}

// ---------------------------------------------------------------------------
// Draws

void FillUniforms(UniformBlock& u) {
  const State& s = g->state;
  D3DMATRIX wv = Multiply(s.world, s.view);
  D3DMATRIX wvp = Multiply(wv, s.projection);
  std::memcpy(u.wvp, &wvp, 64);
  std::memcpy(u.wv, &wv, 64);
  std::memcpy(u.world, &s.world, 64);
  for (int i = 0; i < kMaxTextureSlots; ++i) std::memcpy(u.tex[i], &s.texture[i], 64);
  u.viewport[0] = static_cast<float>(s.viewport.X);
  u.viewport[1] = static_cast<float>(s.viewport.Y);
  u.viewport[2] = static_cast<float>(std::max<DWORD>(s.viewport.Width, 1));
  u.viewport[3] = static_cast<float>(std::max<DWORD>(s.viewport.Height, 1));
  ValueToFloats(s.material.Diffuse, u.matDiffuse);
  ValueToFloats(s.material.Ambient, u.matAmbient);
  ValueToFloats(s.material.Specular, u.matSpecular);
  ValueToFloats(s.material.Emissive, u.matEmissive);
  ColorToFloats(s.rs[D3DRS_AMBIENT], u.globalAmbient);
  ColorToFloats(s.rs[D3DRS_FOGCOLOR], u.fogColor);
  float start = AsFloat(s.rs[D3DRS_FOGSTART]), end = AsFloat(s.rs[D3DRS_FOGEND]);
  u.fogParams[0] = start;
  u.fogParams[1] = end;
  u.fogParams[2] = AsFloat(s.rs[D3DRS_FOGDENSITY]);
  u.fogParams[3] = end != start ? 1.0f / (end - start) : 0.0f;
  ColorToFloats(s.rs[D3DRS_TEXTUREFACTOR], u.tfactor);
  u.misc[0] = (s.rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
  u.misc[1] = s.material.Power;
  // Camera position from the (rigid) view matrix: -t * R^T.
  const D3DMATRIX& v = s.view;
  for (int j = 0; j < 3; ++j)
    u.cameraPos[j] = -(v.m[3][0] * v.m[j][0] + v.m[3][1] * v.m[j][1] + v.m[3][2] * v.m[j][2]);
  std::memcpy(u.psConst, s.psConst, sizeof(u.psConst));
  int n = 0;
  for (const auto& [index, light] : s.lights) {
    if (!light.enabled || light.light.Type != D3DLIGHT_DIRECTIONAL || n >= kMaxLights) continue;
    const D3DVECTOR& d = light.light.Direction;
    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    len = len > 0 ? len : 1;
    u.lightDir[n][0] = d.x / len;
    u.lightDir[n][1] = d.y / len;
    u.lightDir[n][2] = d.z / len;
    ValueToFloats(light.light.Diffuse, u.lightDiffuse[n]);
    ValueToFloats(light.light.Ambient, u.lightAmbient[n]);
    ValueToFloats(light.light.Specular, u.lightSpecular[n]);
    ++n;
  }
}

int CountLights() {
  int n = 0;
  for (const auto& [index, light] : g->state.lights)
    if (light.enabled && light.light.Type == D3DLIGHT_DIRECTIONAL) ++n;
  return std::min(n, kMaxLights);
}

bool BuildKey(const Decl& decl, D3DPRIMITIVETYPE type, bool indexed, PipelineKey& k) {
  const State& s = g->state;
  const DWORD* rs = s.rs;
  std::memset(&k, 0, sizeof(k));
  FixedFunctionKey& ff = k.ff;
  const ParsedShader* vs = nullptr;
  if (s.vertexShader) {
    auto it = g->shaderIds.find(s.vertexShader);
    vs = it != g->shaderIds.end() ? GetShader(it->second) : nullptr;
    if (!vs || !vs->ok) return false;
    ff.vertexShader = it->second;
    for (const VertexElement& e : decl.elements) {
      for (const ShaderInput& in : vs->inputs) {
        if (in.usage != e.usage || in.usageIndex != e.usageIndex || in.reg >= kMaxShaderInputs) continue;
        if (k.attrCount >= kSlotCount) break;
        k.attrs[k.attrCount++] = {e.stream, in.reg, static_cast<uint8_t>(e.format), e.offset};
        ff.vsInputClass[in.reg] = e.cls;
        ff.vsInputComponents[in.reg] = e.components;
      }
    }
  } else {
    for (const VertexElement& e : decl.elements) {
      if (e.slot == 0xFF || k.attrCount >= kSlotCount) continue;
      k.attrs[k.attrCount++] = {e.stream, e.slot, static_cast<uint8_t>(e.format), e.offset};
      ff.attrClass[e.slot] = e.cls;
      ff.attrComponents[e.slot] = e.components;
    }
    if (!ff.attrClass[kSlotPosition]) return false;
  }
  if (s.pixelShader) {
    auto it = g->shaderIds.find(s.pixelShader);
    const ParsedShader* ps = it != g->shaderIds.end() ? GetShader(it->second) : nullptr;
    if (!ps || !ps->ok) return false;
    ff.pixelShader = it->second;
  }
  for (int i = 0; i < k.attrCount; ++i)
    k.strides[k.attrs[i].stream] = static_cast<uint16_t>(s.streams[k.attrs[i].stream].stride);
  ff.pretransformed = decl.pretransformed;
  ff.lighting = rs[D3DRS_LIGHTING] && !decl.pretransformed && !vs;
  ff.colorVertex = rs[D3DRS_COLORVERTEX] != 0;
  ff.diffuseSource = static_cast<uint8_t>(rs[D3DRS_DIFFUSEMATERIALSOURCE]);
  ff.ambientSource = static_cast<uint8_t>(rs[D3DRS_AMBIENTMATERIALSOURCE]);
  ff.specularSource = static_cast<uint8_t>(rs[D3DRS_SPECULARMATERIALSOURCE]);
  ff.emissiveSource = static_cast<uint8_t>(rs[D3DRS_EMISSIVEMATERIALSOURCE]);
  ff.specularEnable = rs[D3DRS_SPECULARENABLE] != 0;
  ff.normalizeNormals = rs[D3DRS_NORMALIZENORMALS] != 0;
  ff.lightCount = ff.lighting ? static_cast<uint8_t>(CountLights()) : 0;
  if (!ff.lighting) ff.diffuseSource = ff.ambientSource = ff.specularSource = ff.emissiveSource = 0;
  if (rs[D3DRS_FOGENABLE]) {
    DWORD mode = rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE ? rs[D3DRS_FOGTABLEMODE] : rs[D3DRS_FOGVERTEXMODE];
    ff.fogMode = mode == D3DFOG_LINEAR ? 1 : mode == D3DFOG_EXP ? 2 : mode == D3DFOG_EXP2 ? 3 : 0;
    // Vertex fog with a vertex shader uses its oFog output as the fog factor.
    if (vs && rs[D3DRS_FOGTABLEMODE] == D3DFOG_NONE) ff.fogMode = 1, ff.fogFactorFromShader = 1;
  }
  ff.alphaTest = rs[D3DRS_ALPHATESTENABLE] != 0;
  ff.alphaFunc = ff.alphaTest ? static_cast<uint8_t>(rs[D3DRS_ALPHAFUNC]) : 0;
  for (int i = 0; i < kMaxStages; ++i) {
    const DWORD* t = s.tss[i];
    if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
    StageKey& st = ff.stage[i];
    st.colorOp = static_cast<uint8_t>(t[D3DTSS_COLOROP]);
    st.colorArg0 = static_cast<uint8_t>(t[D3DTSS_COLORARG0]);
    st.colorArg1 = static_cast<uint8_t>(t[D3DTSS_COLORARG1]);
    st.colorArg2 = static_cast<uint8_t>(t[D3DTSS_COLORARG2]);
    st.alphaOp = static_cast<uint8_t>(t[D3DTSS_ALPHAOP]);
    st.alphaArg0 = static_cast<uint8_t>(t[D3DTSS_ALPHAARG0]);
    st.alphaArg1 = static_cast<uint8_t>(t[D3DTSS_ALPHAARG1]);
    st.alphaArg2 = static_cast<uint8_t>(t[D3DTSS_ALPHAARG2]);
    st.resultArg = static_cast<uint8_t>(t[D3DTSS_RESULTARG]);
    st.texCoordIndex = static_cast<uint8_t>(t[D3DTSS_TEXCOORDINDEX] & 0xFF);
    st.texCoordGen = static_cast<uint8_t>(t[D3DTSS_TEXCOORDINDEX] >> 16);
    DWORD ttf = t[D3DTSS_TEXTURETRANSFORMFLAGS];
    st.ttfCount = static_cast<uint8_t>(ttf & 0xFF);
    st.ttfProjected = (ttf & D3DTTFF_PROJECTED) != 0;
    Texture* tex = i < kMaxTextureSlots ? TextureOf(s.textures[i]) : nullptr;
    st.hasTexture = tex && tex->view;
    st.opaqueAlpha = tex && tex->opaqueAlpha;
    ff.stageCount = static_cast<uint8_t>(i + 1);
  }
  if (ff.pixelShader) {
    std::memset(ff.stage, 0, sizeof(ff.stage));
    ff.stageCount = 0;
    for (int i = 0; i < kMaxTextureSlots; ++i) {
      Texture* tex = TextureOf(s.textures[i]);
      ff.stage[i].hasTexture = tex && tex->view;
    }
  }

  switch (type) {
    case D3DPT_POINTLIST: k.topology = static_cast<uint8_t>(wgpu::PrimitiveTopology::PointList); break;
    case D3DPT_LINELIST: k.topology = static_cast<uint8_t>(wgpu::PrimitiveTopology::LineList); break;
    case D3DPT_LINESTRIP: k.topology = static_cast<uint8_t>(wgpu::PrimitiveTopology::LineStrip); break;
    case D3DPT_TRIANGLESTRIP: k.topology = static_cast<uint8_t>(wgpu::PrimitiveTopology::TriangleStrip); break;
    default: k.topology = static_cast<uint8_t>(wgpu::PrimitiveTopology::TriangleList); break;  // lists, fans
  }
  if (indexed && (type == D3DPT_TRIANGLESTRIP || type == D3DPT_LINESTRIP))
    k.stripIndex = static_cast<uint8_t>(wgpu::IndexFormat::Uint16);

  k.blendEnable = rs[D3DRS_ALPHABLENDENABLE] != 0;
  if (k.blendEnable) {
    DWORD src = rs[D3DRS_SRCBLEND], dst = rs[D3DRS_DESTBLEND];
    if (src == D3DBLEND_BOTHSRCALPHA) src = D3DBLEND_SRCALPHA, dst = D3DBLEND_INVSRCALPHA;
    if (src == D3DBLEND_BOTHINVSRCALPHA) src = D3DBLEND_INVSRCALPHA, dst = D3DBLEND_SRCALPHA;
    k.srcBlend = static_cast<uint8_t>(src);
    k.dstBlend = static_cast<uint8_t>(dst);
    k.blendOp = static_cast<uint8_t>(rs[D3DRS_BLENDOP]);
    bool separate = rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
    k.srcBlendA = separate ? static_cast<uint8_t>(rs[D3DRS_SRCBLENDALPHA]) : k.srcBlend;
    k.dstBlendA = separate ? static_cast<uint8_t>(rs[D3DRS_DESTBLENDALPHA]) : k.dstBlend;
    k.blendOpA = separate ? static_cast<uint8_t>(rs[D3DRS_BLENDOPALPHA]) : k.blendOp;
  }
  k.colorWrite = static_cast<uint8_t>(rs[D3DRS_COLORWRITEENABLE] & 0xF);
  k.cull = static_cast<uint8_t>(rs[D3DRS_CULLMODE]);
  k.hasDepth = g->passHasDepth;
  if (k.hasDepth) {
    k.zEnable = rs[D3DRS_ZENABLE] != D3DZB_FALSE;
    k.zWrite = rs[D3DRS_ZWRITEENABLE] != 0;
    k.zFunc = static_cast<uint8_t>(rs[D3DRS_ZFUNC]);
    k.stencilEnable = rs[D3DRS_STENCILENABLE] != 0;
    if (k.stencilEnable) {
      k.sFunc = static_cast<uint8_t>(rs[D3DRS_STENCILFUNC]);
      k.sPass = static_cast<uint8_t>(rs[D3DRS_STENCILPASS]);
      k.sFail = static_cast<uint8_t>(rs[D3DRS_STENCILFAIL]);
      k.sZFail = static_cast<uint8_t>(rs[D3DRS_STENCILZFAIL]);
      k.twoSided = rs[D3DRS_TWOSIDEDSTENCILMODE] != 0;
      if (k.twoSided) {
        k.sFuncB = static_cast<uint8_t>(rs[D3DRS_CCW_STENCILFUNC]);
        k.sPassB = static_cast<uint8_t>(rs[D3DRS_CCW_STENCILPASS]);
        k.sFailB = static_cast<uint8_t>(rs[D3DRS_CCW_STENCILFAIL]);
        k.sZFailB = static_cast<uint8_t>(rs[D3DRS_CCW_STENCILZFAIL]);
      }
      k.stencilReadMask = static_cast<uint8_t>(rs[D3DRS_STENCILMASK]);
      k.stencilWriteMask = static_cast<uint8_t>(rs[D3DRS_STENCILWRITEMASK]);
    }
    k.depthBias = static_cast<int32_t>(std::lround(AsFloat(rs[D3DRS_DEPTHBIAS]) * 16777216.0f));
    k.slopeBias = AsFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
  }
  return true;
}

UINT VertexCountFor(D3DPRIMITIVETYPE type, UINT prims) {
  switch (type) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return prims + 2;
    default: return 0;
  }
}

// Appends to a CPU staging ring; returns the offset or SIZE_MAX if it doesn't fit.
size_t Stage(std::vector<uint8_t>& ring, size_t capacity, bool& grow, const void* data, size_t size, size_t align) {
  size_t offset = Align(ring.size(), align);
  if (offset + size > capacity) {
    grow = true;
    return SIZE_MAX;
  }
  ring.resize(offset + Align(size, 4));
  if (data) std::memcpy(ring.data() + offset, data, size);
  return offset;
}

// Common draw path. `firstVertex`/`vertexCount` give the absolute vertex range the
// draw reads (used to copy dynamic vertex data); `indices` is either nullptr
// (non-indexed) or the 16-bit indices to draw, relative to `firstVertex`.
void Draw(D3DPRIMITIVETYPE type, UINT firstVertex, UINT vertexCount, const uint16_t* indices, UINT indexCount,
          ResourceProxy* staticIndexBuffer, UINT staticIndexStart, INT staticBaseVertex) {
  State& s = g->state;
  ++g->stats.draws;
  auto declIt = g->decls.find(s.decl);
  if (declIt == g->decls.end()) {
    ++g->stats.skippedOther;
    return;
  }
  const Decl& decl = declIt->second;
  EnsurePass();
  bool indexed = indices || staticIndexBuffer;
  PipelineKey key;
  if (!BuildKey(decl, type, indexed, key)) {
    ++(s.vertexShader || s.pixelShader ? g->stats.skippedShader : g->stats.skippedOther);
    return;
  }
  uint32_t streamMask = 0;
  for (int i = 0; i < key.attrCount; ++i) streamMask |= 1u << key.attrs[i].stream;
  wgpu::RenderPipeline pipeline = GetPipeline(key);
  if (!pipeline) {
    ++g->stats.waitingForPipeline;
    return;
  }

  UniformBlock uniforms{};
  FillUniforms(uniforms);
  size_t uniformOffset =
      Stage(g->uniformData, g->uniformCapacity, g->growUniform, &uniforms, sizeof(uniforms), 256);
  if (uniformOffset == SIZE_MAX) return;
  size_t constantsOffset = 0;
  if (s.vertexShader) {
    constantsOffset =
        Stage(g->uniformData, g->uniformCapacity, g->growUniform, s.vsConst, kVertexConstantsSize, 256);
    if (constantsOffset == SIZE_MAX) return;
  }

  // Vertex streams. Static buffers are addressed with absolute vertex indices; when
  // every stream is dynamic only the used range is copied and indices are rebased.
  struct Binding {
    wgpu::Buffer buffer;
    uint64_t offset;
  } bindings[4];
  bool anyStatic = staticIndexBuffer != nullptr;
  for (int stream = 0; stream < 4; ++stream) {
    if (!(streamMask & (1u << stream))) continue;
    Buffer* b = BufferOf(s.streams[stream].buffer);
    if (!b || !s.streams[stream].stride || (!b->dynamic && !b->gpu)) {
      ++g->stats.skippedOther;
      return;
    }
    if (!b->dynamic) anyStatic = true;
  }
  for (int stream = 0; stream < 4; ++stream) {
    if (!(streamMask & (1u << stream))) continue;
    const State::Stream& st = s.streams[stream];
    Buffer* b = BufferOf(st.buffer);
    if (!b->dynamic) {
      bindings[stream] = {b->gpu, st.offset};
      continue;
    }
    size_t begin = anyStatic ? 0 : st.offset + static_cast<size_t>(firstVertex) * st.stride;
    size_t end = st.offset + static_cast<size_t>(firstVertex + vertexCount) * st.stride;
    if (end > b->shadow.size() || begin >= end) {
      ++g->stats.skippedOther;
      return;
    }
    size_t at = Stage(g->vertexData, g->vertexCapacity, g->growVertex, b->shadow.data() + begin, end - begin, 4);
    if (at == SIZE_MAX) return;
    bindings[stream] = {g->vertexRing, anyStatic ? at + st.offset : at};
  }

  wgpu::BindGroup bindGroup = GetBindGroup(key.ff);
  wgpu::RenderPassEncoder& pass = g->pass;
  pass.SetPipeline(pipeline);
  uint32_t dynamicOffsets[2] = {static_cast<uint32_t>(uniformOffset), static_cast<uint32_t>(constantsOffset)};
  pass.SetBindGroup(0, bindGroup, 2, dynamicOffsets);
  for (int stream = 0; stream < 4; ++stream)
    if (streamMask & (1u << stream)) pass.SetVertexBuffer(stream, bindings[stream].buffer, bindings[stream].offset);
  const D3DVIEWPORT9& vp = s.viewport;
  float vx = static_cast<float>(std::min<DWORD>(vp.X, g->passWidth));
  float vy = static_cast<float>(std::min<DWORD>(vp.Y, g->passHeight));
  float vw = std::min(static_cast<float>(vp.Width), g->passWidth - vx);
  float vh = std::min(static_cast<float>(vp.Height), g->passHeight - vy);
  if (vw <= 0 || vh <= 0) return;
  pass.SetViewport(vx, vy, vw, vh, std::clamp(vp.MinZ, 0.0f, 1.0f), std::clamp(vp.MaxZ, 0.0f, 1.0f));
  if (key.stencilEnable) pass.SetStencilReference(s.rs[D3DRS_STENCILREF] & 0xFF);
  if (key.blendEnable) {
    float bf[4];
    ColorToFloats(s.rs[D3DRS_BLENDFACTOR], bf);
    wgpu::Color c{bf[0], bf[1], bf[2], bf[3]};
    pass.SetBlendConstant(&c);
  }

  if (staticIndexBuffer) {
    Buffer* ib = BufferOf(staticIndexBuffer);
    pass.SetIndexBuffer(ib->gpu, wgpu::IndexFormat::Uint16);
    pass.DrawIndexed(indexCount, 1, staticIndexStart, staticBaseVertex, 0);
  } else if (indices) {
    size_t at = Stage(g->vertexData, g->vertexCapacity, g->growVertex, indices, indexCount * 2, 4);
    if (at == SIZE_MAX) return;
    pass.SetIndexBuffer(g->vertexRing, wgpu::IndexFormat::Uint16, at, Align(indexCount * 2, 4));
    pass.DrawIndexed(indexCount, 1, 0, anyStatic ? static_cast<INT>(firstVertex) : 0, 0);
  } else {
    pass.Draw(vertexCount, 1, anyStatic ? firstVertex : 0, 0);
  }
  ++g->stats.drawn;
}

// ---------------------------------------------------------------------------
// Presentation

bool ConfigureSurface() {
  RECT rc;
  GetClientRect(g->window, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) return false;
  wgpu::SurfaceCapabilities caps;
  g->surface.GetCapabilities(g->adapter, &caps);
  g->surfaceConfig.device = g->device;
  if (g->surfaceConfig.format == wgpu::TextureFormat::Undefined) g->surfaceConfig.format = caps.formats[0];
  g->surfaceConfig.width = static_cast<uint32_t>(rc.right);
  g->surfaceConfig.height = static_cast<uint32_t>(rc.bottom);
  g->surfaceConfig.presentMode = wgpu::PresentMode::Fifo;
  for (size_t i = 0; i < caps.presentModeCount && !g->vsync; ++i) {
    if (caps.presentModes[i] == wgpu::PresentMode::Immediate) {
      g->surfaceConfig.presentMode = wgpu::PresentMode::Immediate;
      break;
    }
    if (caps.presentModes[i] == wgpu::PresentMode::Mailbox) g->surfaceConfig.presentMode = wgpu::PresentMode::Mailbox;
  }
  g->surface.Configure(&g->surfaceConfig);
  g->surfaceConfigured = true;
  return true;
}

// Copies the finished back buffer into `capture` (blocking; debug captures only).
void ReadBackFrame() {
  g->captureRequested = false;
  uint32_t bytesPerRow = static_cast<uint32_t>(Align(g->width * 4, 256));
  wgpu::BufferDescriptor desc{};
  const size_t bufferSize = static_cast<size_t>(bytesPerRow) * g->height;
  desc.size = bufferSize;
  desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer buffer = g->device.CreateBuffer(&desc);
  wgpu::TexelCopyTextureInfo from{};
  from.texture = g->color;
  wgpu::TexelCopyBufferInfo to{};
  to.buffer = buffer;
  to.layout.bytesPerRow = bytesPerRow;
  to.layout.rowsPerImage = g->height;
  wgpu::Extent3D size{g->width, g->height, 1};
  wgpu::CommandEncoder encoder = g->device.CreateCommandEncoder();
  encoder.CopyTextureToBuffer(&from, &to, &size);
  wgpu::CommandBuffer commands = encoder.Finish();
  g->queue.Submit(1, &commands);
  bool mapped = false;
  g->instance.WaitAny(buffer.MapAsync(wgpu::MapMode::Read, 0, bufferSize, wgpu::CallbackMode::WaitAnyOnly,
                                      [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                        mapped = status == wgpu::MapAsyncStatus::Success;
                                      }),
                      5'000'000'000ull);
  if (!mapped) {
    Logf("[webgpu] frame readback failed\n");
    return;
  }
  auto data = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, bufferSize));
  g->capture.resize(static_cast<size_t>(g->width) * g->height * 4);
  for (UINT row = 0; row < g->height; ++row)
    std::memcpy(g->capture.data() + static_cast<size_t>(row) * g->width * 4, data + static_cast<size_t>(row) * bytesPerRow,
                g->width * 4);
  buffer.Unmap();
  g->captureWidth = g->width;
  g->captureHeight = g->height;
}

// Records the blit (if the window can present), uploads the rings and submits.
void SubmitFrame() {
  EnsureEncoder();
  bool canPresent = g->surfaceConfigured || ConfigureSurface();
  wgpu::SurfaceTexture surfaceTexture;
  if (canPresent) {
    g->surface.GetCurrentTexture(&surfaceTexture);
    canPresent = surfaceTexture.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
                 surfaceTexture.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
    if (!canPresent) g->surfaceConfigured = false;
  }
  if (canPresent) {
    wgpu::BindGroupEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].textureView = g->colorView;
    entries[1].binding = 1;
    entries[1].sampler = g->blitSampler;
    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = g->blitLayout;
    bgDesc.entryCount = 2;
    bgDesc.entries = entries;
    wgpu::BindGroup bindGroup = g->device.CreateBindGroup(&bgDesc);
    wgpu::RenderPassColorAttachment color{};
    color.view = surfaceTexture.texture.CreateView();
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    wgpu::RenderPassEncoder pass = g->encoder.BeginRenderPass(&desc);
    pass.SetPipeline(g->blitPipeline);
    pass.SetBindGroup(0, bindGroup);
    pass.Draw(3);
    pass.End();
  }
  wgpu::CommandBuffer commands = g->encoder.Finish();
  g->encoder = nullptr;
  if (!g->uniformData.empty()) g->queue.WriteBuffer(g->uniformRing, 0, g->uniformData.data(), g->uniformData.size());
  if (!g->vertexData.empty()) g->queue.WriteBuffer(g->vertexRing, 0, g->vertexData.data(), g->vertexData.size());
  g->queue.Submit(1, &commands);
  if (canPresent) g->surface.Present();
  if (g->captureRequested) ReadBackFrame();
}

LRESULT CALLBACK MirrorWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      if (g) g->surfaceConfigured = false;
      return 0;
    case WM_CLOSE:
      ShowWindow(w, SW_HIDE);  // closing the mirror only hides it; the game keeps running
      if (g) g->active = false;
      return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

RECT PickMirrorRect() {
  struct Search {
    RECT secondary{};
    bool found = false;
  } search;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        auto* s = reinterpret_cast<Search*>(data);
        if (!(mi.dwFlags & MONITORINFOF_PRIMARY) && !s->found) {
          s->secondary = mi.rcWork;
          s->found = true;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  RECT area = search.found ? search.secondary : RECT{0, 0, 1600, 900};
  int width = std::min(1600L, area.right - area.left), height = width * 9 / 16;
  int x = area.left + ((area.right - area.left) - width) / 2;
  int y = area.top + ((area.bottom - area.top) - height) / 2;
  return {x, y, x + width, y + height};
}

bool CreateMirrorWindow() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = MirrorWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.lpszClassName = L"AndreasGPUMirror";
  RegisterClassW(&wc);
  RECT r = PickMirrorRect();
  DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&r, style, FALSE);
  g->window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"AndreasGPU mirror (WebGPU)", style, r.left,
                              r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (!g->window) return false;
  ShowWindow(g->window, SW_SHOWNOACTIVATE);
  return true;
}

bool CreateSurface() {
  wgpu::SurfaceSourceWindowsHWND source{};
  source.hinstance = GetModuleHandleW(nullptr);
  source.hwnd = g->window;
  wgpu::SurfaceDescriptor desc{};
  desc.nextInChain = &source;
  g->surface = g->instance.CreateSurface(&desc);
  return ConfigureSurface();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API

bool Active() { return g && g->active; }

void ResetDevice(UINT width, UINT height, BOOL autoDepth) {
  if (!g || !g->device) return;
  EndPass();
  CreateTargets(width, height);
  ResetState(autoDepth);
  g->surfaceConfigured = false;  // the game window may have a new size
}

void TextureCreated(ResourceProxy* proxy, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format) {
  if (!Active()) return;
  auto t = std::make_unique<Texture>();
  t->d3dFormat = format;
  t->width = width;
  t->height = height;
  t->levels = std::max<UINT>(levels, 1);
  t->renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
  bool supported = MapTextureFormat(format, t.get());
  if (t->blockCompressed && ((width & 3) || (height & 3))) supported = false;  // WebGPU needs whole blocks
  if (!supported) {
    proxy->backend = static_cast<BackendObject*>(t.release());  // stays white
    return;
  }
  wgpu::TextureDescriptor desc{};
  desc.size = {width, height, 1};
  desc.format = t->format;
  desc.mipLevelCount = t->renderTarget ? 1 : t->levels;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  if (t->renderTarget) desc.usage |= wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  t->texture = g->device.CreateTexture(&desc);
  t->view = t->texture.CreateView();
  proxy->backend = static_cast<BackendObject*>(t.release());
}

void TextureWritten(ResourceProxy* proxy, UINT level, const D3DLOCKED_RECT& locked, const RECT* rect) {
  if (!Active()) return;
  Texture* t = TextureOf(proxy);
  if (!t || !t->texture || level >= t->levels || t->renderTarget) return;
  UINT levelWidth = std::max<UINT>(t->width >> level, 1), levelHeight = std::max<UINT>(t->height >> level, 1);
  UINT x = rect ? rect->left : 0, y = rect ? rect->top : 0;
  UINT w = rect ? rect->right - rect->left : levelWidth, h = rect ? rect->bottom - rect->top : levelHeight;
  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = t->texture;
  dst.mipLevel = level;
  wgpu::TexelCopyBufferLayout layout{};
  const void* data = locked.pBits;
  size_t dataSize = 0;
  std::vector<uint32_t> converted;
  if (t->blockCompressed) {
    // Whole 4x4 blocks; small mips use the physical (block-rounded) size.
    x &= ~3u, y &= ~3u;
    w = Align(w, 4), h = Align(h, 4);
    layout.bytesPerRow = locked.Pitch;
    layout.rowsPerImage = h / 4;
    dataSize = static_cast<size_t>(locked.Pitch) * (h / 4);
  } else if (t->convert) {
    converted.resize(static_cast<size_t>(w) * h);
    UINT bpp = BytesPerTexel(t->d3dFormat);
    for (UINT row = 0; row < h; ++row) {
      auto src = static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(row) * locked.Pitch;
      for (UINT col = 0; col < w; ++col) converted[row * w + col] = ConvertTexel(t->d3dFormat, src + col * bpp);
    }
    data = converted.data();
    layout.bytesPerRow = w * 4;
    layout.rowsPerImage = h;
    dataSize = converted.size() * 4;
  } else {
    layout.bytesPerRow = locked.Pitch;
    layout.rowsPerImage = h;
    dataSize = static_cast<size_t>(locked.Pitch) * (h - 1) + w * 4;
  }
  dst.origin = {x, y, 0};
  wgpu::Extent3D size{w, h, 1};
  g->queue.WriteTexture(&dst, data, dataSize, &layout, &size);
  ++g->stats.textureUploads;
}

void BufferCreated(ResourceProxy* proxy, UINT length, DWORD usage, D3DPOOL pool, bool index, D3DFORMAT indexFormat) {
  if (!Active()) return;
  auto b = std::make_unique<Buffer>();
  b->length = length;
  b->index = index;
  b->dynamic = pool == D3DPOOL_DEFAULT || (usage & D3DUSAGE_DYNAMIC);
  if (index && indexFormat != D3DFMT_INDEX16) {
    proxy->backend = static_cast<BackendObject*>(b.release());  // 32-bit indices: not used by the game
    return;
  }
  if (b->dynamic || index) b->shadow.resize(length);
  if (!b->dynamic) {
    wgpu::BufferDescriptor desc{};
    desc.size = Align(length, 4);
    desc.usage = wgpu::BufferUsage::CopyDst | (index ? wgpu::BufferUsage::Index : wgpu::BufferUsage::Vertex);
    b->gpu = g->device.CreateBuffer(&desc);
  }
  proxy->backend = static_cast<BackendObject*>(b.release());
}

void BufferWritten(ResourceProxy* proxy, UINT offset, UINT size, const void* data) {
  if (!Active()) return;
  Buffer* b = BufferOf(proxy);
  if (!b || offset >= b->length) return;
  if (size == 0 || offset + size > b->length) size = b->length - offset;
  if (!b->shadow.empty()) std::memcpy(b->shadow.data() + offset, data, size);
  if (b->gpu) {
    // WriteBuffer needs 4-byte alignment; the bytes around the locked range belong
    // to the same D3D buffer.
    size_t begin = offset & ~3u;
    size_t end = std::min<size_t>(Align(offset + size, 4), Align(b->length, 4));
    std::vector<uint8_t> bytes(end - begin, 0);
    const uint8_t* lockBase = static_cast<const uint8_t*>(data) - offset;
    size_t copyEnd = std::min<size_t>(end, b->length);
    std::memcpy(bytes.data(), lockBase + begin, copyEnd - begin);
    g->queue.WriteBuffer(b->gpu, begin, bytes.data(), bytes.size());
  }
}

void ResourceDestroyed(ResourceProxy* proxy) {
  if (!g || !proxy->backend) return;
  State& s = g->state;
  for (auto& t : s.textures)
    if (t == proxy) t = nullptr;
  for (auto& st : s.streams)
    if (st.buffer == proxy) st.buffer = nullptr;
  if (s.indices == proxy) s.indices = nullptr;
  if (s.target == proxy) {
    EndPass();
    s.target = nullptr;
  }
  if (Texture* t = TextureOf(proxy)) {
    // Drop only the cached bind groups that reference this texture.
    for (auto it = g->bindGroups.begin(); it != g->bindGroups.end();) {
      bool uses = false;
      for (uint32_t id : it->first.texture) uses |= id == t->id;
      it = uses ? g->bindGroups.erase(it) : std::next(it);
    }
  }
  delete static_cast<BackendObject*>(proxy->backend);
  proxy->backend = nullptr;
}

void VertexDeclarationCreated(const void* declPtr, const D3DVERTEXELEMENT9* e) {
  if (!Active()) return;
  Decl decl;
  for (; e->Stream != 0xFF; ++e) {
    if (e->Usage == D3DDECLUSAGE_POSITIONT) decl.pretransformed = true;
    int slot = SlotForUsage(e->Usage, e->UsageIndex);
    VertexFormatInfo info;
    if (e->Stream >= 4 || !MapDeclType(e->Type, &info)) continue;
    decl.elements.push_back({static_cast<uint8_t>(e->Stream), static_cast<uint8_t>(slot < 0 ? 0xFF : slot), e->Usage,
                             e->UsageIndex, e->Offset, info.format, info.cls, info.components});
    decl.streamMask |= 1u << e->Stream;
  }
  g->decls[declPtr] = std::move(decl);
}

void SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if (Active() && static_cast<DWORD>(state) < 256) g->state.rs[state] = value;
}

void SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (Active() && stage < 8 && static_cast<DWORD>(type) < 33) g->state.tss[stage][type] = value;
}

void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (Active() && sampler < 8 && static_cast<DWORD>(type) < 14) g->state.samp[sampler][type] = value;
}

void SetTexture(DWORD stage, ResourceProxy* texture) {
  if (Active() && stage < 8) g->state.textures[stage] = texture;
}

void SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
  if (!Active() || !m) return;
  State& s = g->state;
  if (state == D3DTS_WORLD) s.world = *m;
  else if (state == D3DTS_VIEW) s.view = *m;
  else if (state == D3DTS_PROJECTION) s.projection = *m;
  else if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7) s.texture[state - D3DTS_TEXTURE0] = *m;
}

void SetLight(DWORD index, const D3DLIGHT9* light) {
  if (Active() && light) g->state.lights[index].light = *light;
}

void LightEnable(DWORD index, BOOL enable) {
  if (Active()) g->state.lights[index].enabled = enable != FALSE;
}

void SetMaterial(const D3DMATERIAL9* material) {
  if (Active() && material) g->state.material = *material;
}

void SetViewport(const D3DVIEWPORT9* viewport) {
  if (Active() && viewport) g->state.viewport = *viewport;
}

void SetStreamSource(UINT stream, ResourceProxy* buffer, UINT offset, UINT stride) {
  if (!Active() || stream >= 4) return;
  g->state.streams[stream] = {buffer, offset, stride};
}

void SetIndices(ResourceProxy* buffer) {
  if (Active()) g->state.indices = buffer;
}

void SetVertexDeclaration(const void* decl) {
  if (Active()) g->state.decl = decl;
}

void SetFVF(DWORD) {
  if (Active()) g->state.decl = nullptr;  // the game uses declarations only
}

void ShaderCreated(const void* shader, const DWORD* code, bool vertex) {
  if (!Active() || !shader || !code) return;
  uint32_t id = RegisterShader(code, vertex);
  g->shaderIds[shader] = id;
  const ParsedShader* parsed = GetShader(id);
  if (parsed && !parsed->ok)
    Logf("[webgpu] %s shader %u not translated: %s\n", vertex ? "vertex" : "pixel", id, parsed->error.c_str());
}

void SetVertexShaderConstantF(UINT start, const float* data, UINT count) {
  if (!Active() || !data || start >= 256) return;
  count = std::min<UINT>(count, 256 - start);
  std::memcpy(g->state.vsConst[start], data, count * 16);
}

void SetPixelShaderConstantF(UINT start, const float* data, UINT count) {
  if (!Active() || !data || start >= 8) return;
  count = std::min<UINT>(count, 8 - start);
  std::memcpy(g->state.psConst[start], data, count * 16);
}

void SetVertexShader(const void* shader) {
  if (Active()) g->state.vertexShader = shader;
}

void SetPixelShader(const void* shader) {
  if (Active()) g->state.pixelShader = shader;
}

void SetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
  if (!Active() || index != 0) return;
  ResourceProxy* target = nullptr;
  if (surface) {
    IDirect3DTexture9* container = nullptr;
    if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&container)))) {
      target = FindProxy(container);
      container->Release();
    }
  }
  if (target != g->state.target) {
    EndPass();
    g->state.target = target;
  }
  // D3D resets the viewport to the new target.
  UINT width, height;
  TargetView(&width, &height);
  g->state.viewport = {0, 0, width, height, 0.0f, 1.0f};
}

void SetDepthStencilSurface(IDirect3DSurface9* surface) {
  if (!Active()) return;
  bool bound = surface != nullptr;
  if (bound != g->state.depthBound) {
    EndPass();
    g->state.depthBound = bound;
  }
}

void Clear(DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
  if (!Active()) return;
  BeginPass(flags, color, z, stencil);
}

void DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) {
  if (!Active()) return;
  UINT count = VertexCountFor(type, primitiveCount);
  if (!count) return;
  if (type == D3DPT_TRIANGLEFAN) {
    std::vector<uint16_t> indices;
    indices.reserve(primitiveCount * 3);
    for (UINT i = 0; i < primitiveCount; ++i) {
      indices.push_back(0);
      indices.push_back(static_cast<uint16_t>(i + 1));
      indices.push_back(static_cast<uint16_t>(i + 2));
    }
    Draw(type, startVertex, count, indices.data(), static_cast<UINT>(indices.size()), nullptr, 0, 0);
    return;
  }
  Draw(type, startVertex, count, nullptr, 0, nullptr, 0, 0);
}

void DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex,
                          UINT primitiveCount) {
  if (!Active()) return;
  Buffer* ib = BufferOf(g->state.indices);
  UINT count = VertexCountFor(type, primitiveCount);
  if (!ib || !count) return;
  UINT firstVertex = static_cast<UINT>(baseVertex + static_cast<INT>(minIndex));
  if (type == D3DPT_TRIANGLEFAN || ib->dynamic) {
    // Copy (and for fans, expand) the indices, rebased to the copied vertex range.
    if ((static_cast<size_t>(startIndex) + count) * 2 > ib->shadow.size()) return;
    auto src = reinterpret_cast<const uint16_t*>(ib->shadow.data()) + startIndex;
    std::vector<uint16_t> indices;
    if (type == D3DPT_TRIANGLEFAN) {
      for (UINT i = 0; i < primitiveCount; ++i) {
        indices.push_back(static_cast<uint16_t>(src[0] - minIndex));
        indices.push_back(static_cast<uint16_t>(src[i + 1] - minIndex));
        indices.push_back(static_cast<uint16_t>(src[i + 2] - minIndex));
      }
    } else {
      indices.resize(count);
      for (UINT i = 0; i < count; ++i) indices[i] = static_cast<uint16_t>(src[i] - minIndex);
    }
    Draw(type, firstVertex, numVertices, indices.data(), static_cast<UINT>(indices.size()), nullptr, 0, 0);
    return;
  }
  Draw(type, firstVertex, numVertices, nullptr, count, g->state.indices, startIndex, baseVertex);
}

// The surface's backing texture: a render-target texture, or the back buffer.
bool ResolveSurface(IDirect3DSurface9* surface, wgpu::Texture* texture, UINT* width, UINT* height) {
  if (!surface) return false;
  IDirect3DTexture9* container = nullptr;
  if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&container)))) {
    Texture* t = TextureOf(FindProxy(container));
    container->Release();
    if (!t || !t->texture || !t->renderTarget) return false;
    *texture = t->texture;
    *width = t->width;
    *height = t->height;
    return true;
  }
  *texture = g->color;
  *width = g->width;
  *height = g->height;
  return true;
}

void StretchRect(IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* dest, const RECT* destRect) {
  if (!Active()) return;
  wgpu::Texture src, dst;
  UINT sw, sh, dw, dh;
  if (!ResolveSurface(source, &src, &sw, &sh) || !ResolveSurface(dest, &dst, &dw, &dh) || src.Get() == dst.Get())
    return;
  RECT s = sourceRect ? *sourceRect : RECT{0, 0, static_cast<LONG>(sw), static_cast<LONG>(sh)};
  RECT d = destRect ? *destRect : RECT{0, 0, static_cast<LONG>(dw), static_cast<LONG>(dh)};
  s.right = std::min<LONG>(s.right, sw), s.bottom = std::min<LONG>(s.bottom, sh);
  d.right = std::min<LONG>(d.right, dw), d.bottom = std::min<LONG>(d.bottom, dh);
  LONG width = s.right - s.left, height = s.bottom - s.top;
  if (width <= 0 || height <= 0) return;
  if (width != d.right - d.left || height != d.bottom - d.top) {
    static bool reported = false;
    if (!reported) Logf("[webgpu] scaled StretchRect not implemented yet (%ldx%ld -> %ldx%ld)\n", width, height,
                        d.right - d.left, d.bottom - d.top);
    reported = true;
    return;
  }
  // Post effects copy the back buffer into a texture they then draw back with a filter.
  EndPass();
  EnsureEncoder();
  wgpu::TexelCopyTextureInfo from{};
  from.texture = src;
  from.origin = {static_cast<uint32_t>(s.left), static_cast<uint32_t>(s.top), 0};
  wgpu::TexelCopyTextureInfo to{};
  to.texture = dst;
  to.origin = {static_cast<uint32_t>(d.left), static_cast<uint32_t>(d.top), 0};
  wgpu::Extent3D size{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  g->encoder.CopyTextureToTexture(&from, &to, &size);
}

void RequestCapture() {
  if (Active()) g->captureRequested = true;
}

bool TakeCapture(std::vector<uint8_t>* bgra, UINT* width, UINT* height) {
  if (!Active() || g->capture.empty()) return false;
  *bgra = std::move(g->capture);
  g->capture.clear();
  *width = g->captureWidth;
  *height = g->captureHeight;
  return true;
}

void Present() {
  if (!Active()) return;
  EndPass();
  SubmitFrame();
  g->uniformData.clear();
  g->vertexData.clear();
  EnsureRings();  // grows after submission so recorded draws keep their buffers
  g->instance.ProcessEvents();
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (g->lastPresent.QuadPart) {
    double ms = (now.QuadPart - g->lastPresent.QuadPart) * 1000.0 / freq.QuadPart;
    g->stats.totalFrameMs += ms;
    g->stats.worstFrameMs = std::max(g->stats.worstFrameMs, ms);
    if (ms > 25.0) ++g->stats.slowFrames;
  }
  g->lastPresent = now;
  if (!g->warmupDone) {
    size_t compiling = 0;
    for (const auto& [key, pipeline] : g->pipelines) compiling += pipeline ? 0 : 1;
    if (compiling == 0) {
      g->warmupDone = true;
      Logf("[webgpu] pipeline cache: %zu pipelines ready %.0f ms after startup\n", g->warmupCount,
           (now.QuadPart - g->warmupStart.QuadPart) * 1000.0 / freq.QuadPart);
    }
  }
  if (++g->frame % 600 == 0) {
    Logf("[webgpu] frame %u: avg %.1f ms, worst %.1f ms, %u frames >25 ms | draws=%u drawn=%u waiting=%u "
         "skipped(shaders)=%u skipped(other)=%u | new pipelines=%u (total %zu) bindgroups=%zu uploads=%u\n",
         g->frame, g->stats.totalFrameMs / 600.0, g->stats.worstFrameMs, g->stats.slowFrames, g->stats.draws,
         g->stats.drawn, g->stats.waitingForPipeline, g->stats.skippedShader, g->stats.skippedOther,
         g->stats.pipelinesCreated, g->pipelines.size(), g->bindGroups.size(), g->stats.textureUploads);
    LogFlush();
    g->stats = {};
  }
}

}  // namespace agpu::wgb

// ---------------------------------------------------------------------------
// Mirror entry points (webgpu/mirror.h)

namespace agpu::mirror {

using agpu::wgb::g;

bool Start(HWND presentWindow, bool vsync) {
  using namespace agpu::wgb;
  if (g) return g->active;
  g = new Backend();
  g->vsync = vsync;
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instanceDesc{};
  instanceDesc.requiredFeatureCount = 1;
  instanceDesc.requiredFeatures = &kTimedWaitAny;
  g->instance = wgpu::CreateInstance(&instanceDesc);
  if (!g->instance) return false;
  wgpu::RequestAdapterOptions adapterOptions{};
  adapterOptions.backendType = wgpu::BackendType::D3D12;
  adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;
  g->instance.WaitAny(g->instance.RequestAdapter(&adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                                                 [](wgpu::RequestAdapterStatus status, wgpu::Adapter a,
                                                    wgpu::StringView message) {
                                                   if (status == wgpu::RequestAdapterStatus::Success)
                                                     g->adapter = std::move(a);
                                                   else
                                                     Logf("[mirror] RequestAdapter: %s\n", Str(message).c_str());
                                                 }),
                      UINT64_MAX);
  if (!g->adapter) return false;
  wgpu::DeviceDescriptor deviceDesc{};
  wgpu::FeatureName features[] = {wgpu::FeatureName::TextureCompressionBC};
  deviceDesc.requiredFeatureCount = 1;
  deviceDesc.requiredFeatures = features;
  deviceDesc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        Logf("[mirror] device lost (%d): %s\n", static_cast<int>(reason), Str(message).c_str());
        if (g) g->active = false;
      });
  deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
    static int reported = 0;
    if (reported++ < 20) Logf("[mirror] WebGPU error (%d): %s\n", static_cast<int>(type), Str(message).c_str());
  });
  g->instance.WaitAny(g->adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                               [](wgpu::RequestDeviceStatus status, wgpu::Device d,
                                                  wgpu::StringView message) {
                                                 if (status == wgpu::RequestDeviceStatus::Success)
                                                   g->device = std::move(d);
                                                 else
                                                   Logf("[mirror] RequestDevice: %s\n", Str(message).c_str());
                                               }),
                      UINT64_MAX);
  if (!g->device) return false;
  g->queue = g->device.GetQueue();
  if (presentWindow) {
    g->window = presentWindow;
    g->presentsInGameWindow = true;
  } else if (!CreateMirrorWindow()) {
    return false;
  }
  if (!CreateSurface()) return false;
  CreateFixedObjects();
  EnsureRings();
  g->active = true;
  WarmUpPipelineCache();
  Logf("[webgpu] started: presenting in %s, %ux%u, present mode %d\n",
       presentWindow ? "the game window" : "the mirror window", g->surfaceConfig.width, g->surfaceConfig.height,
       static_cast<int>(g->surfaceConfig.presentMode));
  LogFlush();
  return true;
}

bool Active() { return agpu::wgb::Active(); }

void OnPresent() { agpu::wgb::Present(); }

}  // namespace agpu::mirror
