// See recorder.h.
#include "recorder.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gen/d3d9_proxy.h"
#include "config.h"
#include "game_hooks.h"
#include "log.h"

namespace agpu {
namespace rec {
namespace {

// ---------------------------------------------------------------------------
// Names for the dump

const char* Name(const char* const* table, size_t count, DWORD v) {
  return v < count && table[v] ? table[v] : nullptr;
}
std::string NameOr(const char* const* table, size_t count, DWORD v) {
  if (const char* n = Name(table, count, v)) return n;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", v);
  return buf;
}
#define NAMED(table, v) NameOr(table, std::size(table), v)

const char* const kPrim[] = {nullptr, "POINTLIST", "LINELIST", "LINESTRIP", "TRILIST", "TRISTRIP", "TRIFAN"};
const char* const kTop[] = {nullptr, "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X",
                            "MODULATE4X", "ADD", "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH",
                            "BLENDDIFFUSEALPHA", "BLENDTEXTUREALPHA", "BLENDFACTORALPHA",
                            "BLENDTEXTUREALPHAPM", "BLENDCURRENTALPHA", "PREMODULATE",
                            "MODULATEALPHA_ADDCOLOR", "MODULATECOLOR_ADDALPHA",
                            "MODULATEINVALPHA_ADDCOLOR", "MODULATEINVCOLOR_ADDALPHA", "BUMPENVMAP",
                            "BUMPENVMAPLUMINANCE", "DOTPRODUCT3", "MULTIPLYADD", "LERP"};
const char* const kTa[] = {"DIFFUSE", "CURRENT", "TEXTURE", "TFACTOR", "SPECULAR", "TEMP", "CONSTANT"};
const char* const kBlendNames[] = {nullptr, "ZERO", "ONE", "SRCCOLOR", "INVSRCCOLOR", "SRCALPHA", "INVSRCALPHA",
                              "DESTALPHA", "INVDESTALPHA", "DESTCOLOR", "INVDESTCOLOR", "SRCALPHASAT",
                              "BOTHSRCALPHA", "BOTHINVSRCALPHA", "BLENDFACTOR", "INVBLENDFACTOR"};
const char* const kCmp[] = {nullptr, "NEVER", "LESS", "EQUAL", "LESSEQUAL", "GREATER", "NOTEQUAL",
                            "GREATEREQUAL", "ALWAYS"};
const char* const kAddr[] = {nullptr, "WRAP", "MIRROR", "CLAMP", "BORDER", "MIRRORONCE"};
const char* const kFilter[] = {"NONE", "POINT", "LINEAR", "ANISOTROPIC", nullptr, nullptr, "PYRAMIDALQUAD",
                               "GAUSSIANQUAD"};
const char* const kDeclType[] = {"FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4", "SHORT2",
                                 "SHORT4", "UBYTE4N", "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N",
                                 "UDEC3", "DEC3N", "FLOAT16_2", "FLOAT16_4", "UNUSED"};
const char* const kDeclUsage[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                                  "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                                  "COLOR", "FOG", "DEPTH", "SAMPLE"};

std::string TaName(DWORD v) {
  std::string s = NAMED(kTa, v & D3DTA_SELECTMASK);
  if (v & D3DTA_COMPLEMENT) s = "1-" + s;
  if (v & D3DTA_ALPHAREPLICATE) s += ".a";
  return s;
}

std::string FmtName(DWORD f) {
  switch (f) {
    case D3DFMT_UNKNOWN: return "-";
    case D3DFMT_R8G8B8: return "R8G8B8";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    case D3DFMT_A8: return "A8";
    case D3DFMT_P8: return "P8";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_V8U8: return "V8U8";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D32: return "D32";
    case D3DFMT_INDEX16: return "INDEX16";
    case D3DFMT_INDEX32: return "INDEX32";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
  }
  if (f > 0xFFFF) {  // FOURCC (DXT1..5, ...)
    char s[5] = {char(f), char(f >> 8), char(f >> 16), char(f >> 24), 0};
    return s;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "fmt%lu", f);
  return buf;
}

std::string TexCoordIndexName(DWORD v) {
  std::string s = std::to_string(v & 0xFFFF);
  switch (v & 0xFFFF0000) {
    case D3DTSS_TCI_CAMERASPACENORMAL: s += "|CAMERASPACENORMAL"; break;
    case D3DTSS_TCI_CAMERASPACEPOSITION: s += "|CAMERASPACEPOSITION"; break;
    case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: s += "|CAMERASPACEREFLECTIONVECTOR"; break;
    case D3DTSS_TCI_SPHEREMAP: s += "|SPHEREMAP"; break;
    case 0: break;
    default: s += "|gen" + std::to_string(v >> 16);
  }
  return s;
}
// ---------------------------------------------------------------------------
// Tracked device state

struct NamedState {
  D3DRENDERSTATETYPE state;
  const char* name;
};

// Render states that affect how a draw must be translated. Values that are
// irrelevant in context (e.g. blend factors with blending off) are zeroed in the key.
enum KeyRs {
  kZEnable, kZWrite, kZFunc, kCull, kFill, kShade, kColorWrite,
  kBlend, kSrcBlend, kDstBlend, kBlendOp, kSepAlpha, kSrcBlendA, kDstBlendA, kBlendOpA,
  kAlphaTest, kAlphaFunc, kAlphaRef,
  kStencil, kStencilFunc, kStencilPass, kStencilFail, kStencilZFail,
  kFog, kFogTable, kFogVertex, kRangeFog,
  kSpecular, kClipPlanes, kDepthBias, kSlopeBias, kSrgbWrite, kPointSprite,
  // Fixed-function vertex processing only (zeroed when a vertex shader is bound).
  kLighting, kColorVertex, kDiffuseSrc, kAmbientSrc, kSpecularSrc, kEmissiveSrc, kNormalize,
  kLocalViewer, kVertexBlend, kIndexedBlend,
  kKeyRsCount
};

const NamedState kKeyRsInfo[kKeyRsCount] = {
    {D3DRS_ZENABLE, "ZENABLE"}, {D3DRS_ZWRITEENABLE, "ZWRITE"}, {D3DRS_ZFUNC, "ZFUNC"},
    {D3DRS_CULLMODE, "CULL"}, {D3DRS_FILLMODE, "FILL"}, {D3DRS_SHADEMODE, "SHADE"},
    {D3DRS_COLORWRITEENABLE, "COLORWRITE"},
    {D3DRS_ALPHABLENDENABLE, "BLEND"}, {D3DRS_SRCBLEND, "SRC"}, {D3DRS_DESTBLEND, "DST"},
    {D3DRS_BLENDOP, "BLENDOP"}, {D3DRS_SEPARATEALPHABLENDENABLE, "SEPALPHA"},
    {D3DRS_SRCBLENDALPHA, "SRCA"}, {D3DRS_DESTBLENDALPHA, "DSTA"}, {D3DRS_BLENDOPALPHA, "BLENDOPA"},
    {D3DRS_ALPHATESTENABLE, "ATEST"}, {D3DRS_ALPHAFUNC, "AFUNC"}, {D3DRS_ALPHAREF, "AREF"},
    {D3DRS_STENCILENABLE, "STENCIL"}, {D3DRS_STENCILFUNC, "SFUNC"}, {D3DRS_STENCILPASS, "SPASS"},
    {D3DRS_STENCILFAIL, "SFAIL"}, {D3DRS_STENCILZFAIL, "SZFAIL"},
    {D3DRS_FOGENABLE, "FOG"}, {D3DRS_FOGTABLEMODE, "FOGTABLE"}, {D3DRS_FOGVERTEXMODE, "FOGVERTEX"},
    {D3DRS_RANGEFOGENABLE, "RANGEFOG"},
    {D3DRS_SPECULARENABLE, "SPECULAR"}, {D3DRS_CLIPPLANEENABLE, "CLIPPLANES"},
    {D3DRS_DEPTHBIAS, "DEPTHBIAS"}, {D3DRS_SLOPESCALEDEPTHBIAS, "SLOPEBIAS"},
    {D3DRS_SRGBWRITEENABLE, "SRGBWRITE"}, {D3DRS_POINTSPRITEENABLE, "POINTSPRITE"},
    {D3DRS_LIGHTING, "LIGHTING"}, {D3DRS_COLORVERTEX, "COLORVERTEX"},
    {D3DRS_DIFFUSEMATERIALSOURCE, "DIFFUSESRC"}, {D3DRS_AMBIENTMATERIALSOURCE, "AMBIENTSRC"},
    {D3DRS_SPECULARMATERIALSOURCE, "SPECULARSRC"}, {D3DRS_EMISSIVEMATERIALSOURCE, "EMISSIVESRC"},
    {D3DRS_NORMALIZENORMALS, "NORMALIZE"}, {D3DRS_LOCALVIEWER, "LOCALVIEWER"},
    {D3DRS_VERTEXBLEND, "VERTEXBLEND"}, {D3DRS_INDEXEDVERTEXBLENDENABLE, "INDEXEDBLEND"},
};

constexpr DWORD kStages = 8;
constexpr DWORD kTssCount = 33;      // D3DTSS_CONSTANT + 1
constexpr DWORD kSamplerCount = 14;  // D3DSAMP_DMAPOFFSET + 1

struct DeviceState {
  DWORD rs[256];
  DWORD tss[kStages][kTssCount];
  DWORD samp[kStages][kSamplerCount];
  IDirect3DBaseTexture9* tex[kStages];
  DWORD fvf;
  IDirect3DVertexDeclaration9* decl;
  IDirect3DVertexShader9* vs;
  IDirect3DPixelShader9* ps;
  DWORD streamMask;
  std::map<DWORD, D3DLIGHTTYPE> lightType;
  std::set<DWORD> lightsOn;
} g_state;

void ResetStateToDefaults(BOOL autoDepth) {
  DeviceState& s = g_state;
  std::memset(s.rs, 0, sizeof(s.rs));
  s.rs[D3DRS_ZENABLE] = autoDepth ? D3DZB_TRUE : D3DZB_FALSE;
  s.rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  s.rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  s.rs[D3DRS_ZWRITEENABLE] = TRUE;
  s.rs[D3DRS_LASTPIXEL] = TRUE;
  s.rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  s.rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  s.rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  s.rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  s.rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  s.rs[D3DRS_STENCILFAIL] = s.rs[D3DRS_STENCILZFAIL] = s.rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  s.rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  s.rs[D3DRS_STENCILMASK] = s.rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  s.rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
  s.rs[D3DRS_CLIPPING] = TRUE;
  s.rs[D3DRS_LIGHTING] = TRUE;
  s.rs[D3DRS_COLORVERTEX] = TRUE;
  s.rs[D3DRS_LOCALVIEWER] = TRUE;
  s.rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  s.rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  s.rs[D3DRS_COLORWRITEENABLE] = 0xF;
  s.rs[D3DRS_BLENDOP] = s.rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  s.rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  s.rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  for (DWORD i = 0; i < kStages; ++i) {
    DWORD* t = s.tss[i];
    std::memset(t, 0, sizeof(s.tss[i]));
    t[D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    t[D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    t[D3DTSS_COLORARG2] = t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    t[D3DTSS_COLORARG0] = t[D3DTSS_ALPHAARG0] = t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
    t[D3DTSS_TEXCOORDINDEX] = i;
    DWORD* sm = s.samp[i];
    std::memset(sm, 0, sizeof(s.samp[i]));
    sm[D3DSAMP_ADDRESSU] = sm[D3DSAMP_ADDRESSV] = sm[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    sm[D3DSAMP_MAGFILTER] = sm[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    sm[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    s.tex[i] = nullptr;
  }
  s.fvf = 0;
  s.decl = nullptr;
  s.vs = nullptr;
  s.ps = nullptr;
  s.streamMask = 0;
  s.lightType.clear();
  s.lightsOn.clear();
}

// ---------------------------------------------------------------------------
// Object ids (shaders/decls are referred to by small ids in the dumps)

std::unordered_map<void*, uint32_t> g_declId, g_vsId, g_psId;
std::vector<std::string> g_declText;                // by id - 1
std::vector<std::string> g_vsText, g_psText;        // by id - 1

uint32_t IdOf(const std::unordered_map<void*, uint32_t>& m, void* p) {
  if (!p) return 0;
  auto it = m.find(p);
  return it == m.end() ? 0xFFFFFFFF : it->second;
}

D3DFORMAT TextureFormat(IDirect3DBaseTexture9* t, DWORD* type) {
  *type = 0;
  if (!t) return D3DFMT_UNKNOWN;
  *type = t->GetType();
  D3DSURFACE_DESC d{};
  if (*type == D3DRTYPE_TEXTURE)
    static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
  else if (*type == D3DRTYPE_CUBETEXTURE)
    static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d);
  return d.Format;
}

// ---------------------------------------------------------------------------
// Draw keys

const char* const kDrawApi[] = {"DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
                                "DrawIndexedPrimitiveUP"};

struct StageKey {
  DWORD colorOp, colorArg0, colorArg1, colorArg2;
  DWORD alphaOp, alphaArg0, alphaArg1, alphaArg2;
  DWORD resultArg, texCoordIndex, ttf;
  DWORD texType, texFormat;
  DWORD addrU, addrV, magFilter, minFilter, mipFilter;
};

struct DrawKey {
  uint8_t api, prim, stageCount, fixedVertex;
  DWORD fvf, declId, vsId, psId, streamMask;
  DWORD rs[kKeyRsCount];
  StageKey stage[kStages];
  uint8_t lights[4];  // enabled lights by D3DLIGHTTYPE (index 1..3)
};

struct DrawKeyHash {
  size_t operator()(const DrawKey& k) const {
    auto p = reinterpret_cast<const uint8_t*>(&k);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(k); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
  }
};
struct DrawKeyEq {
  bool operator()(const DrawKey& a, const DrawKey& b) const { return std::memcmp(&a, &b, sizeof(a)) == 0; }
};

constexpr int kCallerDepth = 3;
constexpr int kMaxCallChains = 4;
using CallChain = std::array<uintptr_t, kCallerDepth>;

struct DrawStats {
  uint64_t draws = 0;
  uint64_t prims = 0;
  uint32_t firstFrame = 0, lastFrame = 0;
  std::vector<CallChain> chains;
};

std::unordered_map<DrawKey, DrawStats, DrawKeyHash, DrawKeyEq> g_draws;
uint32_t g_frame = 0;
uint64_t g_primTotals[7] = {};

// Return addresses into gta_sa.exe code (.text + _rwcseg), innermost first. Heuristic
// stack scan: a value counts if the bytes before it decode as a call instruction.
CallChain ScanCallers(const uintptr_t* sp) {
  CallChain out{};
  int n = 0;
  for (int i = 0; i < 96 && n < kCallerDepth; ++i) {
    uintptr_t v = sp[i];
    if (v < 0x401000 || v >= 0x857000) continue;
    auto p = reinterpret_cast<const uint8_t*>(v);
    bool isCall = p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
                  (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10);
    if (isCall) out[n++] = v;
  }
  return out;
}

void RecordDraw(DrawApi api, D3DPRIMITIVETYPE prim, UINT primCount, const uintptr_t* sp) {
  const DeviceState& s = g_state;
  DrawKey k;
  std::memset(&k, 0, sizeof(k));
  k.api = api;
  k.prim = static_cast<uint8_t>(prim);
  k.fvf = s.fvf;
  k.declId = IdOf(g_declId, s.decl);
  k.vsId = IdOf(g_vsId, s.vs);
  k.psId = IdOf(g_psId, s.ps);
  k.streamMask = s.streamMask;
  k.fixedVertex = s.vs == nullptr;

  for (int i = 0; i < kKeyRsCount; ++i) k.rs[i] = s.rs[kKeyRsInfo[i].state];
  if (!k.rs[kBlend]) k.rs[kSrcBlend] = k.rs[kDstBlend] = k.rs[kBlendOp] = 0;
  if (!k.rs[kBlend] || !k.rs[kSepAlpha]) k.rs[kSrcBlendA] = k.rs[kDstBlendA] = k.rs[kBlendOpA] = 0;
  if (!k.rs[kAlphaTest]) k.rs[kAlphaFunc] = k.rs[kAlphaRef] = 0;
  if (!k.rs[kStencil]) k.rs[kStencilFunc] = k.rs[kStencilPass] = k.rs[kStencilFail] = k.rs[kStencilZFail] = 0;
  if (!k.rs[kFog]) k.rs[kFogTable] = k.rs[kFogVertex] = k.rs[kRangeFog] = 0;
  if (!k.fixedVertex) {
    for (int i = kLighting; i < kKeyRsCount; ++i) k.rs[i] = 0;
  } else if (!k.rs[kLighting]) {
    for (int i = kColorVertex; i <= kLocalViewer; ++i) k.rs[i] = 0;
  }

  // Stages: texture bindings matter either way; combiners only without a pixel shader.
  for (DWORD i = 0; i < kStages; ++i) {
    const DWORD* t = s.tss[i];
    if (!s.ps && t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
    if (s.ps && !s.tex[i]) continue;
    StageKey& st = k.stage[i];
    k.stageCount = static_cast<uint8_t>(i + 1);
    if (!s.ps) {
      st.colorOp = t[D3DTSS_COLOROP];
      st.colorArg0 = t[D3DTSS_COLORARG0];
      st.colorArg1 = t[D3DTSS_COLORARG1];
      st.colorArg2 = t[D3DTSS_COLORARG2];
      st.alphaOp = t[D3DTSS_ALPHAOP];
      st.alphaArg0 = t[D3DTSS_ALPHAARG0];
      st.alphaArg1 = t[D3DTSS_ALPHAARG1];
      st.alphaArg2 = t[D3DTSS_ALPHAARG2];
      st.resultArg = t[D3DTSS_RESULTARG];
    }
    if (k.fixedVertex) {
      st.texCoordIndex = t[D3DTSS_TEXCOORDINDEX];
      st.ttf = t[D3DTSS_TEXTURETRANSFORMFLAGS];
    }
    st.texFormat = TextureFormat(s.tex[i], &st.texType);
    if (s.tex[i]) {
      st.addrU = s.samp[i][D3DSAMP_ADDRESSU];
      st.addrV = s.samp[i][D3DSAMP_ADDRESSV];
      st.magFilter = s.samp[i][D3DSAMP_MAGFILTER];
      st.minFilter = s.samp[i][D3DSAMP_MINFILTER];
      st.mipFilter = s.samp[i][D3DSAMP_MIPFILTER];
    }
  }

  if (k.fixedVertex && s.rs[D3DRS_LIGHTING]) {
    for (DWORD idx : s.lightsOn) {
      auto it = s.lightType.find(idx);
      DWORD type = it == s.lightType.end() ? 0 : it->second;
      if (type < 4 && k.lights[type] < 255) ++k.lights[type];
    }
  }

  DrawStats& st = g_draws[k];
  if (st.draws++ == 0) st.firstFrame = g_frame;
  st.lastFrame = g_frame;
  st.prims += primCount;
  if (st.chains.size() < kMaxCallChains) {
    CallChain c = ScanCallers(sp);
    if (std::find(st.chains.begin(), st.chains.end(), c) == st.chains.end()) st.chains.push_back(c);
  }
  if (prim < 7) g_primTotals[prim] += primCount;
}

// ---------------------------------------------------------------------------
// Resource summaries

std::map<std::string, uint64_t> g_resources;  // description -> count
uint64_t g_calls[static_cast<size_t>(DeviceCall::Count)] = {};
std::map<DWORD, uint64_t> g_transforms;  // D3DTRANSFORMSTATETYPE -> SetTransform calls

std::string DescribeDecl(const D3DVERTEXELEMENT9* e) {
  std::string s;
  for (; e->Stream != 0xFF; ++e) {
    char buf[96];
    snprintf(buf, sizeof(buf), "  stream%u +%-3u %-9s %s%u\n", e->Stream, e->Offset,
             NAMED(kDeclType, e->Type).c_str(), NAMED(kDeclUsage, e->Usage).c_str(), e->UsageIndex);
    s += buf;
  }
  return s;
}

size_t ShaderSize(const DWORD* code) {
  size_t n = 1;
  while (code[n] != 0x0000FFFF) ++n;
  return (n + 1) * sizeof(DWORD);
}

std::string SurfDesc(IDirect3DSurface9* s) {
  if (!s) return "null";
  D3DSURFACE_DESC d{};
  s->GetDesc(&d);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %ux%u", FmtName(d.Format).c_str(), d.Width, d.Height);
  return buf;
}

// Shader bytecode is deduplicated by content; the id is stable for identical code.
uint32_t RegisterShader(std::vector<std::string>& texts, const DWORD* code, const char* prefix,
                        const char* origin) {
  std::string bytes(reinterpret_cast<const char*>(code), ShaderSize(code));
  auto it = std::find(texts.begin(), texts.end(), bytes);
  if (it != texts.end()) return static_cast<uint32_t>(it - texts.begin()) + 1;
  texts.push_back(bytes);
  uint32_t id = static_cast<uint32_t>(texts.size());
  wchar_t name[64];
  swprintf(name, 64, L"%hs_%03u.bin", prefix, id);
  WriteLogFile(name, bytes.data(), bytes.size());
  Logf("[shader] %s_%03u: %s, %u bytes, version %08lX\n", prefix, id, origin,
       static_cast<unsigned>(bytes.size()), code[0]);
  return id;
}

void LogDeviceCaps(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type) {
  D3DCAPS9 c{};
  if (FAILED(d3d->GetDeviceCaps(adapter, type, &c))) return;
  Logf("[caps] VS %08lX PS %08lX MaxTextureBlendStages=%lu MaxSimultaneousTextures=%lu MaxActiveLights=%lu "
       "MaxVertexShaderConst=%lu TextureOpCaps=%08lX PrimitiveMiscCaps=%08lX TextureCaps=%08lX\n",
       c.VertexShaderVersion, c.PixelShaderVersion, c.MaxTextureBlendStages, c.MaxSimultaneousTextures,
       c.MaxActiveLights, c.MaxVertexShaderConst, c.TextureOpCaps, c.PrimitiveMiscCaps, c.TextureCaps);
}

// ---------------------------------------------------------------------------
// Dump

void Appendf(std::string& s, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s += buf;
}

std::string DescribeDrawKey(const DrawKey& k) {
  std::string s;
  Appendf(s, "  %s %s  fvf=%08lX decl=%lu vs=%lu ps=%lu streams=%lX\n", kDrawApi[k.api],
          NAMED(kPrim, k.prim).c_str(), k.fvf, k.declId, k.vsId, k.psId, k.streamMask);
  s += "  rs:";
  for (int i = 0; i < kKeyRsCount; ++i) {
    DWORD v = k.rs[i];
    if (!v && i != kZEnable && i != kZWrite && i != kBlend && i != kLighting) continue;
    std::string val;
    if (i == kSrcBlend || i == kDstBlend || i == kSrcBlendA || i == kDstBlendA)
      val = NAMED(kBlendNames, v);
    else if (i == kZFunc || i == kAlphaFunc || i == kStencilFunc)
      val = NAMED(kCmp, v);
    else
      val = std::to_string(v);
    Appendf(s, " %s=%s", kKeyRsInfo[i].name, val.c_str());
  }
  s += "\n";
  for (int i = 0; i < k.stageCount; ++i) {
    const StageKey& st = k.stage[i];
    Appendf(s, "  stage%d:", i);
    if (!k.psId) {
      Appendf(s, " color=%s(%s,%s) alpha=%s(%s,%s)", NAMED(kTop, st.colorOp).c_str(),
              TaName(st.colorArg1).c_str(), TaName(st.colorArg2).c_str(), NAMED(kTop, st.alphaOp).c_str(),
              TaName(st.alphaArg1).c_str(), TaName(st.alphaArg2).c_str());
      if (st.colorOp == D3DTOP_MULTIPLYADD || st.colorOp == D3DTOP_LERP)
        Appendf(s, " colorArg0=%s", TaName(st.colorArg0).c_str());
      if (st.resultArg != D3DTA_CURRENT) Appendf(s, " result=%s", TaName(st.resultArg).c_str());
    }
    if (k.fixedVertex) {
      Appendf(s, " tci=%s", TexCoordIndexName(st.texCoordIndex).c_str());
      if (st.ttf) Appendf(s, " ttf=%lX", st.ttf);
    }
    if (st.texType)
      Appendf(s, " tex=%s%s addr=%s/%s filter=%s/%s/%s", st.texType == D3DRTYPE_CUBETEXTURE ? "cube:" : "",
              FmtName(st.texFormat).c_str(), NAMED(kAddr, st.addrU).c_str(), NAMED(kAddr, st.addrV).c_str(),
              NAMED(kFilter, st.magFilter).c_str(), NAMED(kFilter, st.minFilter).c_str(),
              NAMED(kFilter, st.mipFilter).c_str());
    else
      s += " tex=none";
    s += "\n";
  }
  if (k.lights[1] || k.lights[2] || k.lights[3])
    Appendf(s, "  lights: point=%u spot=%u directional=%u\n", k.lights[D3DLIGHT_POINT], k.lights[D3DLIGHT_SPOT],
            k.lights[D3DLIGHT_DIRECTIONAL]);
  return s;
}

}  // namespace

bool Enabled() { return GetConfig().record; }

void Dump(const char* reason) {
  if (!Enabled()) return;
  std::vector<std::pair<const DrawKey*, const DrawStats*>> rows;
  for (auto& [k, v] : g_draws) rows.push_back({&k, &v});
  std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second->draws > b.second->draws; });

  std::string s;
  Appendf(s, "# andreasgpu draw states (%s) frame=%u unique=%u\n", reason, g_frame,
          static_cast<unsigned>(rows.size()));
  s += "# callers = return addresses in gta_sa.exe, innermost first (heuristic stack scan)\n\n";
  int n = 0;
  for (auto& [k, v] : rows) {
    Appendf(s, "#%d draws=%llu prims=%llu frames=%u..%u\n", n++, v->draws, v->prims, v->firstFrame, v->lastFrame);
    for (const CallChain& c : v->chains)
      Appendf(s, "  callers: %08X <- %08X <- %08X\n", static_cast<unsigned>(c[0]), static_cast<unsigned>(c[1]),
              static_cast<unsigned>(c[2]));
    s += DescribeDrawKey(*k);
    s += "\n";
  }
  WriteLogFile(L"draw_states.txt", s.data(), s.size());

  s.clear();
  Appendf(s, "# andreasgpu summary (%s) frame=%u\n\n## primitives drawn\n", reason, g_frame);
  for (int i = 1; i < 7; ++i) Appendf(s, "  %-10s %llu\n", kPrim[i], g_primTotals[i]);
  s += "\n## device calls (all methods, non-zero)\n";
  for (size_t i = 0; i < std::size(g_calls); ++i)
    if (g_calls[i]) Appendf(s, "  %-28s %llu\n", kDeviceCallNames[i], g_calls[i]);
  s += "\n## SetTransform by state (2=VIEW 3=PROJECTION 16..23=TEXTUREn 256=WORLD)\n";
  uint64_t extraWorld = 0;
  for (auto& [t, count] : g_transforms) {
    if (t > D3DTS_WORLD)
      extraWorld += count;  // D3DTS_WORLDMATRIX(1..255), only used for vertex blending
    else
      Appendf(s, "  %4lu %llu\n", t, count);
  }
  if (extraWorld) Appendf(s, "  WORLDMATRIX(1..255) %llu total\n", extraWorld);
  s += "\n## resources and targets\n";
  for (auto& [what, count] : g_resources) Appendf(s, "  %8llu  %s\n", count, what.c_str());
  s += "\n## vertex declarations\n";
  for (size_t i = 0; i < g_declText.size(); ++i) Appendf(s, "decl %u:\n%s", static_cast<unsigned>(i + 1),
                                                          g_declText[i].c_str());
  Appendf(s, "\n## shaders: %u vertex, %u pixel (bytecode in vs_NNN.bin / ps_NNN.bin)\n",
          static_cast<unsigned>(g_vsText.size()), static_cast<unsigned>(g_psText.size()));
  WriteLogFile(L"summary.txt", s.data(), s.size());
  Logf("[dump] %s at frame %u: %u draw states\n", reason, g_frame, static_cast<unsigned>(rows.size()));
  LogFlush();
}

void ResetState(BOOL autoDepthStencil) { ResetStateToDefaults(autoDepthStencil); }

void SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if (static_cast<DWORD>(state) < 256) g_state.rs[state] = value;
}

void SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage < kStages && static_cast<DWORD>(type) < kTssCount) g_state.tss[stage][type] = value;
}

void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (sampler < kStages && static_cast<DWORD>(type) < kSamplerCount) g_state.samp[sampler][type] = value;
}

void SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
  if (stage < kStages) g_state.tex[stage] = texture;
}

void SetFvf(DWORD fvf) {
  g_state.fvf = fvf;
  g_state.decl = nullptr;
}

void SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) {
  g_state.decl = decl;
  g_state.fvf = 0;
}

void SetVertexShader(IDirect3DVertexShader9* shader) { g_state.vs = shader; }

void SetPixelShader(IDirect3DPixelShader9* shader) { g_state.ps = shader; }

void SetStreamSource(UINT stream, bool bound) {
  if (stream >= 32) return;
  if (bound)
    g_state.streamMask |= 1u << stream;
  else
    g_state.streamMask &= ~(1u << stream);
}

void SetLight(DWORD index, const D3DLIGHT9* light) {
  if (!light) return;
  g_state.lightType[index] = light->Type;
  Resource("SetLight type=%u attenuation=(%g,%g,%g)", light->Type, light->Attenuation0, light->Attenuation1,
           light->Attenuation2);
}

void LightEnable(DWORD index, BOOL enable) {
  if (enable)
    g_state.lightsOn.insert(index);
  else
    g_state.lightsOn.erase(index);
}

void SetTransform(D3DTRANSFORMSTATETYPE state) { ++g_transforms[state]; }

void Draw(DrawApi api, D3DPRIMITIVETYPE prim, UINT primCount, const uintptr_t* sp) {
  if (!Enabled()) return;
  RecordDraw(api, prim, primCount, sp);
}

void OnPresent() {
  constexpr uint32_t kDumpEveryFrames = 3600;
  if (++g_frame % kDumpEveryFrames == 0) Dump("periodic");
}

void VertexDeclarationCreated(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9* decl) {
  std::string text = DescribeDecl(elements);
  auto it = std::find(g_declText.begin(), g_declText.end(), text);
  if (it == g_declText.end()) it = g_declText.insert(g_declText.end(), text);
  g_declId[decl] = static_cast<uint32_t>(it - g_declText.begin()) + 1;
}

void VertexShaderCreated(const DWORD* code, IDirect3DVertexShader9* shader, uintptr_t caller) {
  uint32_t desc = 0;
  char origin[64];
  if (InVertexShaderGenerator(&desc))
    snprintf(origin, sizeof(origin), "RW generator desc=%08X", desc);
  else
    snprintf(origin, sizeof(origin), "caller %08X", static_cast<unsigned>(caller));
  g_vsId[shader] = RegisterShader(g_vsText, code, "vs", origin);
}

void PixelShaderCreated(const DWORD* code, IDirect3DPixelShader9* shader, uintptr_t caller) {
  char origin[32];
  snprintf(origin, sizeof(origin), "caller %08X", static_cast<unsigned>(caller));
  g_psId[shader] = RegisterShader(g_psText, code, "ps", origin);
}

void Resource(const char* fmt, ...) {
  if (!Enabled()) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ++g_resources[buf];
}

std::string FormatName(DWORD format) { return FmtName(format); }

std::string SurfaceDesc(IDirect3DSurface9* surface) { return SurfDesc(surface); }

std::string FilterName(DWORD filter) { return NAMED(kFilter, filter); }

void LogCaps(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type) { LogDeviceCaps(d3d, adapter, type); }

}  // namespace rec

void CountDeviceCall(DeviceCall call) { ++rec::g_calls[static_cast<size_t>(call)]; }

}  // namespace agpu
