// See ffshader.h. D3D9 semantics follow the D3D9 fixed-function documentation:
// lighting in world space (equivalent to D3D's camera space for rigid transforms),
// colour clamping per vertex, texture stage combiners per component, specular added
// after the stages, then fog, then alpha test.
#include "webgpu/ffshader.h"

#include <cstdarg>
#include <cstdio>

#include "webgpu/shader_translator.h"

namespace agpu::wgb {
namespace {

// D3DTA_*
constexpr uint8_t kTaSelectMask = 0x0F, kTaComplement = 0x10, kTaAlphaReplicate = 0x20;
enum : uint8_t { kTaDiffuse = 0, kTaCurrent = 1, kTaTexture = 2, kTaTFactor = 3, kTaSpecular = 4, kTaTemp = 5 };
// D3DTSS_TCI_* >> 16
enum : uint8_t { kGenPassThru = 0, kGenCameraNormal = 1, kGenCameraPosition = 2, kGenReflection = 3, kGenSphereMap = 4 };

void Append(std::string& s, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s += buf;
}

const char* kUniforms = R"(struct U {
  wvp: mat4x4f, wv: mat4x4f, world: mat4x4f,
  tex0: mat4x4f, tex1: mat4x4f, tex2: mat4x4f, tex3: mat4x4f,
  viewport: vec4f,
  matDiffuse: vec4f, matAmbient: vec4f, matSpecular: vec4f, matEmissive: vec4f,
  globalAmbient: vec4f, fogColor: vec4f, fogParams: vec4f, tfactor: vec4f, misc: vec4f, cameraPos: vec4f, cameraBack: vec4f,
  lightDir: array<vec4f, 4>, lightDiffuse: array<vec4f, 4>, lightAmbient: array<vec4f, 4>, lightSpecular: array<vec4f, 4>,
  psConst: array<vec4f, 8>,
};
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var t0Tex: texture_2d<f32>;
@group(0) @binding(2) var s0: sampler;
@group(0) @binding(3) var t1Tex: texture_2d<f32>;
@group(0) @binding(4) var s1: sampler;
@group(0) @binding(5) var t2Tex: texture_2d<f32>;
@group(0) @binding(6) var s2: sampler;
@group(0) @binding(7) var t3Tex: texture_2d<f32>;
@group(0) @binding(8) var s3: sampler;
@group(0) @binding(9) var<uniform> vc: array<vec4f, 256>;

struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) diffuse: vec4f,
  @location(1) specular: vec4f,
  @location(2) uv0: vec4f,
  @location(3) uv1: vec4f,
  @location(4) uv2: vec4f,
  @location(5) uv3: vec4f,
  @location(6) fogDepth: f32,
  @location(7) fogFactor: f32,
};
)";

std::string AttrType(const FixedFunctionKey& k, int slot) {
  static const char* kFloat[] = {"", "f32", "vec2f", "vec3f", "vec4f"};
  static const char* kSint[] = {"", "i32", "vec2i", "vec3i", "vec4i"};
  static const char* kUint[] = {"", "u32", "vec2u", "vec3u", "vec4u"};
  int n = k.attrComponents[slot];
  switch (k.attrClass[slot]) {
    case kAttrSint: return kSint[n];
    case kAttrUint: return kUint[n];
    default: return kFloat[n];
  }
}

// Attribute widened to vec4f with D3D defaults for missing components (0, 0, 0, 1).
std::string AttrVec4(const FixedFunctionKey& k, int slot, const char* name) {
  int n = k.attrComponents[slot];
  std::string v = k.attrClass[slot] == kAttrFloat ? std::string("in.") + name
                                                  : "vec" + std::to_string(n) + "f(in." + name + ")";
  if (n == 1) v = "vec2f(" + v + ", 0.0)", n = 2;
  switch (n) {
    case 2: return "vec4f(" + v + ", 0.0, 1.0)";
    case 3: return "vec4f(" + v + ", 1.0)";
    default: return v;
  }
}

// Texture coordinate as D3D9 feeds it to a texture transform: the first missing
// component is 1 and the rest 0, so (u, v) becomes (u, v, 1, 0) and 2D matrices
// keep their translation in the third row (_31, _32).
std::string TransformInput(const FixedFunctionKey& k, int slot, const char* name) {
  int n = k.attrComponents[slot];
  std::string v = k.attrClass[slot] == kAttrFloat ? std::string("in.") + name
                                                  : "vec" + std::to_string(n) + "f(in." + name + ")";
  switch (n) {
    case 1: return "vec4f(" + v + ", 1.0, 0.0, 0.0)";
    case 2: return "vec4f(" + v + ", 1.0, 0.0)";
    case 3: return "vec4f(" + v + ", 1.0)";
    default: return v;
  }
}

std::string MaterialSource(const FixedFunctionKey& k, uint8_t source, const char* material) {
  if (k.colorVertex) {
    if (source == 1 && k.attrClass[kSlotColor0]) return "in.c0";
    if (source == 2 && k.attrClass[kSlotColor1]) return "in.c1";
  }
  return std::string("u.") + material;
}

void EmitVertex(std::string& s, const FixedFunctionKey& k) {
  s += "struct VIn {\n";
  static const char* kNames[kSlotCount] = {"pos", "normal", "c0", "c1", "uv0", "uv1", "uv2", "uv3"};
  for (int slot = 0; slot < kSlotCount; ++slot)
    if (k.attrClass[slot]) Append(s, "  @location(%d) %s: %s,\n", slot, kNames[slot], AttrType(k, slot).c_str());
  s += "};\n\n@vertex fn vs(in: VIn) -> VOut {\n  var out: VOut;\n";

  if (k.pretransformed) {
    // XYZRHW in render-target pixels; D3D9 pixel centres sit on integers.
    s += "  let w = select(1.0 / in.pos.w, 1.0, in.pos.w == 0.0);\n"
         "  let ndc = vec2f((in.pos.x + 0.5 - u.viewport.x) / u.viewport.z * 2.0 - 1.0,\n"
         "                  1.0 - (in.pos.y + 0.5 - u.viewport.y) / u.viewport.w * 2.0);\n"
         "  out.pos = vec4f(ndc * w, in.pos.z * w, w);\n"
         "  let viewPos = vec3f(0.0);\n";
  } else {
    s += "  let p = vec4f(in.pos.xyz, 1.0);\n"
         "  out.pos = u.wvp * p;\n"
         // D3D9 pixel centres sit on integers: move half a pixel right and down.
         "  out.pos = vec4f(out.pos.xy + vec2f(1.0 / u.viewport.z, -1.0 / u.viewport.w) * out.pos.w, out.pos.zw);\n"
         "  let viewPos = (u.wv * p).xyz;\n";
  }
  bool hasNormal = k.attrClass[kSlotNormal] != kAttrNone && !k.pretransformed;
  if (hasNormal) s += "  let viewNormal = normalize((u.wv * vec4f(in.normal, 0.0)).xyz);\n";

  if (k.lighting && !k.pretransformed) {
    std::string dif = MaterialSource(k, k.diffuseSource, "matDiffuse");
    std::string amb = MaterialSource(k, k.ambientSource, "matAmbient");
    std::string spe = MaterialSource(k, k.specularSource, "matSpecular");
    std::string emi = MaterialSource(k, k.emissiveSource, "matEmissive");
    Append(s, "  let cDif = %s;\n  let cAmb = %s;\n  let cSpe = %s;\n  let cEmi = %s;\n", dif.c_str(), amb.c_str(),
           spe.c_str(), emi.c_str());
    s += "  var ambient = u.globalAmbient.rgb;\n  var diffuse = vec3f(0.0);\n  var specular = vec3f(0.0);\n";
    if (hasNormal && k.lightCount) {
      s += "  let worldPos = (u.world * p).xyz;\n"
           "  let n = normalize((u.world * vec4f(in.normal, 0.0)).xyz);\n";
      // Without D3DRS_LOCALVIEWER, D3D9 uses (0, 0, -1) in camera space (measured).
      s += k.localViewer ? "  let toEye = normalize(u.cameraPos.xyz - worldPos);\n"
                         : "  let toEye = u.cameraBack.xyz;\n";
      Append(s, "  for (var i = 0; i < %d; i++) {\n", k.lightCount);
      s += "    let l = -u.lightDir[i].xyz;\n"
           "    let ndl = dot(n, l);\n"
           "    ambient += u.lightAmbient[i].rgb;\n"
           "    diffuse += max(ndl, 0.0) * u.lightDiffuse[i].rgb;\n";
      if (k.specularEnable)
        s += "    if (ndl > 0.0) { specular += pow(max(dot(n, normalize(l + toEye)), 0.0), u.misc.y) * "
             "u.lightSpecular[i].rgb; }\n";
      s += "  }\n";
    } else if (k.lightCount) {
      Append(s, "  for (var i = 0; i < %d; i++) { ambient += u.lightAmbient[i].rgb; }\n", k.lightCount);
    }
    s += "  out.diffuse = saturate(vec4f(cAmb.rgb * ambient + cDif.rgb * diffuse + cEmi.rgb, cDif.a));\n"
         "  out.specular = saturate(vec4f(cSpe.rgb * specular, 0.0));\n";
  } else {
    s += k.attrClass[kSlotColor0] ? "  out.diffuse = in.c0;\n" : "  out.diffuse = vec4f(1.0);\n";
    s += k.attrClass[kSlotColor1] ? "  out.specular = in.c1;\n" : "  out.specular = vec4f(0.0);\n";
  }

  for (int i = 0; i < kMaxTextureSlots; ++i) {
    const StageKey& st = k.stage[i];
    std::string coord = "vec4f(0.0, 0.0, 0.0, 1.0)";
    if (i < k.stageCount && st.hasTexture) {
      switch (st.texCoordGen) {
        case kGenCameraNormal:
          if (hasNormal) coord = "vec4f(viewNormal, 1.0)";
          break;
        case kGenCameraPosition: coord = "vec4f(viewPos, 1.0)"; break;
        case kGenReflection:
          if (hasNormal)
            coord = "vec4f(reflect(normalize(viewPos), viewNormal), 1.0)";
          break;
        case kGenSphereMap:
          if (hasNormal) coord = "vec4f(viewNormal.xy * 0.5 + 0.5, 0.0, 1.0)";
          break;
        default: {
          int slot = kSlotTexcoord0 + (st.texCoordIndex & 3);
          static const char* kUv[] = {"uv0", "uv1", "uv2", "uv3"};
          if (k.attrClass[slot])
            coord = st.ttfCount ? TransformInput(k, slot, kUv[st.texCoordIndex & 3])
                                : AttrVec4(k, slot, kUv[st.texCoordIndex & 3]);
        }
      }
      if (st.ttfCount) {
        Append(s, "  let tc%d = u.tex%d * %s;\n", i, i, coord.c_str());
        if (st.ttfProjected)
          Append(s, "  out.uv%d = vec4f(tc%d.xy / tc%d[%d], 0.0, 1.0);\n", i, i, i, st.ttfCount - 1);
        else
          Append(s, "  out.uv%d = tc%d;\n", i, i);
        continue;
      }
    }
    Append(s, "  out.uv%d = %s;\n", i, coord.c_str());
  }
  s += "  out.fogDepth = viewPos.z;\n  out.fogFactor = 1.0;\n  return out;\n}\n\n";
}

std::string Arg(const FixedFunctionKey& k, int stage, uint8_t arg) {
  std::string v;
  switch (arg & kTaSelectMask) {
    case kTaDiffuse: v = "in.diffuse"; break;
    case kTaCurrent: v = stage == 0 ? "in.diffuse" : "cur"; break;
    case kTaTexture:
      v = stage < kMaxTextureSlots && k.stage[stage].hasTexture ? "tex" + std::to_string(stage) : "vec4f(1.0)";
      break;
    case kTaTFactor: v = "u.tfactor"; break;
    case kTaSpecular: v = "in.specular"; break;
    case kTaTemp: v = "tmp"; break;
    default: v = "u.tfactor"; break;  // D3DTA_CONSTANT is not used by the game
  }
  if (arg & kTaComplement) v = "(vec4f(1.0) - " + v + ")";
  if (arg & kTaAlphaReplicate) v = "vec4f(" + v + ".a)";
  return v;
}

// Expression for one D3DTEXTUREOP on component suffix `c` (".rgb" or ".a").
std::string Op(const FixedFunctionKey& k, int stage, uint8_t op, uint8_t arg0, uint8_t arg1, uint8_t arg2,
               const char* c) {
  std::string a0 = Arg(k, stage, arg0) + c, a1 = Arg(k, stage, arg1) + c, a2 = Arg(k, stage, arg2) + c;
  auto alphaOf = [&](const std::string& v) { return "(" + v + ").a"; };
  std::string tex = stage < kMaxTextureSlots && k.stage[stage].hasTexture ? "tex" + std::to_string(stage) : "vec4f(1.0)";
  std::string cur = stage == 0 ? "in.diffuse" : "cur";
  std::string arg1Full = Arg(k, stage, arg1);
  switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 + " * " + a2;
    case 5: return a1 + " * " + a2 + " * 2.0";
    case 6: return a1 + " * " + a2 + " * 4.0";
    case 7: return a1 + " + " + a2;
    case 8: return a1 + " + " + a2 + " - 0.5";
    case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
    case 10: return a1 + " - " + a2;
    case 11: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
    case 12: return "mix(" + a2 + ", " + a1 + ", " + alphaOf("in.diffuse") + ")";
    case 13: return "mix(" + a2 + ", " + a1 + ", " + alphaOf(tex) + ")";
    case 14: return "mix(" + a2 + ", " + a1 + ", u.tfactor.a)";
    case 15: return a1 + " + " + a2 + " * (1.0 - " + alphaOf(tex) + ")";
    case 16: return "mix(" + a2 + ", " + a1 + ", " + alphaOf(cur) + ")";
    case 18: return a1 + " + " + alphaOf(arg1Full) + " * " + a2;
    case 19: return a1 + " * " + a2 + " + " + alphaOf(arg1Full);
    case 20: return "(1.0 - " + alphaOf(arg1Full) + ") * " + a2 + " + " + a1;
    case 21: return "(1.0 - " + a1 + ") * " + a2 + " + " + alphaOf(arg1Full);
    case 24: return "dot(" + Arg(k, stage, arg1) + ".rgb - 0.5, " + Arg(k, stage, arg2) + ".rgb - 0.5) * 4.0";
    case 25: return a0 + " + " + a1 + " * " + a2;
    case 26: return a0 + " * " + a1 + " + (1.0 - " + a0 + ") * " + a2;
    default: return a1;  // PREMODULATE, BUMPENVMAP*: closest is arg1
  }
}

void EmitStages(std::string& s, const FixedFunctionKey& k);
void EmitFogAndAlphaTest(std::string& s, const FixedFunctionKey& k);

void EmitFragment(std::string& s, const FixedFunctionKey& k) {
  s += "@fragment fn fs(in: VOut) -> @location(0) vec4f {\n";
  if (const ParsedShader* ps = k.pixelShader ? GetShader(k.pixelShader) : nullptr) {
    s += TranslatePixelShader(*ps);
  } else {
    EmitStages(s, k);
  }
  EmitFogAndAlphaTest(s, k);
}

void EmitStages(std::string& s, const FixedFunctionKey& k) {
  for (int i = 0; i < kMaxTextureSlots; ++i) {
    if (i >= k.stageCount || !k.stage[i].hasTexture) continue;
    // With a vertex shader the stage reads oT[TEXCOORDINDEX]; the fixed-function
    // vertex stage already wrote the coordinate for stage i into uv{i}.
    int uv = k.vertexShader ? (k.stage[i].texCoordIndex & 3) : i;
    Append(s, "  var tex%d = textureSample(t%dTex, s%d, in.uv%d.xy);\n", i, i, i, uv);
    if (k.stage[i].opaqueAlpha) Append(s, "  tex%d.a = 1.0;\n", i);
  }
  s += "  var cur = in.diffuse;\n  var tmp = vec4f(0.0);\n";
  for (int i = 0; i < k.stageCount; ++i) {
    const StageKey& st = k.stage[i];
    if (st.colorOp <= 1) break;  // D3DTOP_DISABLE
    std::string rgb = Op(k, i, st.colorOp, st.colorArg0, st.colorArg1, st.colorArg2, ".rgb");
    std::string a = st.alphaOp <= 1 ? std::string(i == 0 ? "in.diffuse.a" : "cur.a")
                                    : Op(k, i, st.alphaOp, st.alphaArg0, st.alphaArg1, st.alphaArg2, ".a");
    if (st.colorOp == 24) a = "(" + rgb + ")";  // DOTPRODUCT3 replicates into alpha
    const char* target = (st.resultArg & kTaSelectMask) == kTaTemp ? "tmp" : "cur";
    Append(s, "  %s = saturate(vec4f(vec3f(%s), %s));\n", target, rgb.c_str(), a.c_str());
  }
  if (k.specularEnable) s += "  cur = vec4f(saturate(cur.rgb + in.specular.rgb), cur.a);\n";
}

void EmitFogAndAlphaTest(std::string& s, const FixedFunctionKey& k) {
  if (k.fogMode && k.fogFactorFromShader) {
    s += "  let fog = saturate(in.fogFactor);\n";
  } else {
    switch (k.fogMode) {
    case 1: s += "  let fog = saturate((u.fogParams.y - in.fogDepth) * u.fogParams.w);\n"; break;
    case 2: s += "  let fog = saturate(exp(-u.fogParams.z * in.fogDepth));\n"; break;
    case 3: s += "  let fog = saturate(exp(-pow(u.fogParams.z * in.fogDepth, 2.0)));\n"; break;
    default: break;
    }
  }
  if (k.fogMode) s += "  cur = vec4f(mix(u.fogColor.rgb, cur.rgb, fog), cur.a);\n";
  if (k.alphaTest) {
    // D3D compares 8-bit alpha against D3DRS_ALPHAREF.
    static const char* kCmp[] = {"false", "false", "<", "==", "<=", ">", "!=", ">=", "true"};
    s += "  let a8 = round(cur.a * 255.0);\n  let ref8 = round(u.misc.x * 255.0);\n";
    uint8_t f = k.alphaFunc <= 8 ? k.alphaFunc : 8;
    if (f == 1 || f == 0)
      s += "  discard;\n";
    else if (f != 8)
      Append(s, "  if (!(a8 %s ref8)) { discard; }\n", kCmp[f]);
  }
  s += "  return cur;\n}\n";
}

}  // namespace

std::string GenerateWgsl(const FixedFunctionKey& key) {
  std::string s = kUniforms;
  s += "\n";
  if (const ParsedShader* vs = key.vertexShader ? GetShader(key.vertexShader) : nullptr)
    s += TranslateVertexShader(*vs, key.vsInputClass, key.vsInputComponents);
  else
    EmitVertex(s, key);
  EmitFragment(s, key);
  return s;
}

}  // namespace agpu::wgb
