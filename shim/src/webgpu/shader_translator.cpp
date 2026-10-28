// See shader_translator.h. Token format: D3D9 driver documentation ("Shader Codes"):
//  instruction token: opcode bits 0-15, control 16-23, length 24-27 (SM2+), co-issue 30;
//  parameter token: number 0-10, type bits 28-30 | 11-12, relative addressing bit 13,
//  destination write mask 16-19, result modifier 20-23, shift 24-27;
//  source swizzle 16-23, source modifier 24-27.
#include "webgpu/shader_translator.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace agpu::wgb {
namespace {

enum RegType {
  kTemp = 0, kInput = 1, kConst = 2, kAddrOrTexture = 3, kRastOut = 4, kAttrOut = 5, kTexCrdOut = 6,
  kConstInt = 7, kColorOut = 8, kDepthOut = 9, kSampler = 10, kConstBool = 14,
};

enum Opcode {
  kNop = 0, kMov = 1, kAdd = 2, kSub = 3, kMad = 4, kMul = 5, kRcp = 6, kRsq = 7, kDp3 = 8, kDp4 = 9,
  kMin = 10, kMax = 11, kSlt = 12, kSge = 13, kExp = 14, kLog = 15, kLit = 16, kDst = 17, kLrp = 18, kFrc = 19,
  kM4x4 = 20, kM4x3 = 21, kM3x4 = 22, kM3x3 = 23, kM3x2 = 24, kDcl = 31, kPow = 32, kCrs = 33, kSgn = 34,
  kAbs = 35, kNrm = 36, kSinCos = 37, kMova = 46, kTexCoord = 64, kTexKill = 65, kTex = 66, kExpp = 78,
  kLogp = 79, kCnd = 80, kDef = 81, kCmp = 88, kDp2Add = 90, kPhase = 0xFFFD, kComment = 0xFFFE, kEnd = 0xFFFF,
};

struct Instr {
  uint32_t opcode;
  uint32_t control;
  bool coissue;
  std::vector<DWORD> args;  // parameter tokens (relative-address tokens included inline)
};

std::vector<ParsedShader> g_shaders;  // id = index + 1
std::map<std::vector<DWORD>, uint32_t> g_ids;

uint32_t RegType(DWORD t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
uint32_t RegNum(DWORD t) { return t & 0x7FF; }

// Operand counts for SM1.x, which does not encode instruction length.
int Sm1Length(uint32_t op) {
  switch (op) {
    case kNop:
    case kPhase: return 0;
    case kTex:
    case kTexKill:
    case kTexCoord: return 1;
    case kMov:
    case kRcp:
    case kRsq:
    case kExp:
    case kLog:
    case kLit:
    case kFrc:
    case kExpp:
    case kLogp:
    case kMova: return 2;
    case kAdd:
    case kSub:
    case kMul:
    case kDp3:
    case kDp4:
    case kMin:
    case kMax:
    case kSlt:
    case kSge:
    case kDst:
    case kM4x4:
    case kM4x3:
    case kM3x4:
    case kM3x3:
    case kM3x2: return 3;
    case kMad:
    case kLrp:
    case kCnd:
    case kCmp:
    case kDp2Add: return 4;
    case kDef: return 5;
    case kDcl: return 2;
    default: return -1;
  }
}

bool Parse(ParsedShader& s, std::vector<Instr>* out) {
  const std::vector<DWORD>& t = s.tokens;
  size_t i = 1;
  while (i < t.size()) {
    DWORD tok = t[i];
    uint32_t op = tok & 0xFFFF;
    if (op == kEnd) return true;
    if (op == kComment) {
      i += 1 + ((tok >> 16) & 0x7FFF);
      continue;
    }
    int len = s.major >= 2 ? static_cast<int>((tok >> 24) & 0xF) : Sm1Length(op);
    if (len < 0 || i + 1 + len > t.size()) {
      char buf[64];
      snprintf(buf, sizeof(buf), "unknown opcode %u at token %zu", op, i);
      s.error = buf;
      return false;
    }
    if (s.major < 2) {
      // SM1 relative addressing (vs_1_1 c[a0.x + n]) has no extra token; nothing to add.
    }
    Instr ins{op, (tok >> 16) & 0xFF, (tok & 0x40000000) != 0, {}};
    ins.args.assign(t.begin() + i + 1, t.begin() + i + 1 + len);
    if (out) out->push_back(std::move(ins));
    i += 1 + len;
  }
  s.error = "missing end token";
  return false;
}

void Append(std::string& s, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s += buf;
}

// ---------------------------------------------------------------------------
// Expression building

struct Emitter {
  const ParsedShader& shader;
  bool vertex;
  std::map<uint32_t, std::string> defs;  // constant register -> literal (def)
  std::set<uint32_t> temps, textures;
  std::string body;

  // Reads parameter `index` of `ins`; relative addressing consumes a following
  // token in SM2+ (stored inline in args), so callers walk with `pos`.
  std::string Register(const Instr& ins, size_t& pos, bool* isMatrixBase = nullptr, int rowOffset = 0) {
    DWORD tok = ins.args[pos++];
    uint32_t type = RegType(tok), num = RegNum(tok) + rowOffset;
    bool relative = (tok & (1u << 13)) != 0;
    std::string rel;
    if (relative) {
      std::string component = "x";
      if (shader.major >= 2 && pos < ins.args.size()) {
        DWORD relTok = ins.args[pos++];
        static const char kComp[] = "xyzw";
        component = kComp[(relTok >> 16) & 3];
      }
      rel = "a0." + component;
    }
    (void)isMatrixBase;
    switch (type) {
      case kTemp:
        temps.insert(num);
        return "r" + std::to_string(num);
      case kInput:
        if (vertex) return "v" + std::to_string(num);
        return num == 0 ? "in.diffuse" : "in.specular";
      case kConst: {
        if (!relative) {
          auto it = defs.find(num);
          if (it != defs.end()) return it->second;
          return vertex ? "vc[" + std::to_string(num) + "]" : "u.psConst[" + std::to_string(num & 7) + "]";
        }
        return "vc[clamp(" + rel + " + " + std::to_string(num) + ", 0, 255)]";
      }
      case kAddrOrTexture:
        if (vertex) return "vec4f(a0)";
        textures.insert(num);
        return "t" + std::to_string(num);
      default: return "vec4f(0.0)";
    }
  }

  std::string Source(const Instr& ins, size_t& pos, int rowOffset = 0) {
    DWORD tok = ins.args[pos];
    std::string v = Register(ins, pos, nullptr, rowOffset);
    uint32_t swz = (tok >> 16) & 0xFF;
    if (swz != 0xE4) {  // .xyzw
      static const char kComp[] = "xyzw";
      v = "(" + v + ")." + kComp[swz & 3] + kComp[(swz >> 2) & 3] + kComp[(swz >> 4) & 3] + kComp[(swz >> 6) & 3];
    }
    switch ((tok >> 24) & 0xF) {
      case 1: return "(-" + v + ")";
      case 2: return "(" + v + " - vec4f(0.5))";
      case 3: return "(-(" + v + " - vec4f(0.5)))";
      case 4: return "(" + v + " * 2.0 - vec4f(1.0))";
      case 5: return "(-(" + v + " * 2.0 - vec4f(1.0)))";
      case 6: return "(vec4f(1.0) - " + v + ")";
      case 7: return "(" + v + " * 2.0)";
      case 8: return "(-" + v + " * 2.0)";
      case 11: return "abs(" + v + ")";
      case 12: return "(-abs(" + v + "))";
      default: return v;
    }
  }

  // Scalar operand of rcp/rsq/exp/log: the replicated component (w without swizzle in SM1).
  std::string Scalar(const Instr& ins, size_t& pos) {
    DWORD tok = ins.args[pos];
    std::string v = Source(ins, pos);
    uint32_t swz = (tok >> 16) & 0xFF;
    return "(" + v + ")." + (swz == 0xE4 ? "w" : "x");
  }

  void Assign(DWORD dst, const std::string& value, bool clampPs1) {
    uint32_t type = RegType(dst), num = RegNum(dst);
    uint32_t mask = (dst >> 16) & 0xF;
    uint32_t mod = (dst >> 20) & 0xF;
    int shift = static_cast<int>((dst >> 24) & 0xF);
    std::string v = value;
    if (shift) {
      static const char* kShift[] = {"", "* 2.0", "* 4.0", "* 8.0", "", "", "", "", "", "", "", "", "* 0.0625",
                                     "* 0.125", "* 0.25", "* 0.5"};
      v = "(" + v + " " + kShift[shift] + ")";
    }
    if (mod & 1)
      v = "saturate(" + v + ")";
    else if (clampPs1)
      v = "clamp(" + v + ", vec4f(-1.0), vec4f(1.0))";  // ps_1_x registers hold [-1, 1]
    std::string target;
    bool scalarTarget = false;
    switch (type) {
      case kTemp:
        temps.insert(num);
        target = "r" + std::to_string(num);
        break;
      case kAddrOrTexture:
        if (vertex) {
          v = "vec4i(round(" + v + "))";
          target = "a0";
        } else {
          textures.insert(num);
          target = "t" + std::to_string(num);
        }
        break;
      case kRastOut:
        if (num == 0) target = "out.pos";
        else if (num == 1) target = "out.fogFactor", scalarTarget = true;
        else return;  // point size
        break;
      case kAttrOut: target = num == 0 ? "out.diffuse" : "out.specular"; break;
      case kTexCrdOut:
        if (num > 3) return;
        target = "out.uv" + std::to_string(num);
        break;
      case kColorOut: target = "cur"; break;
      default: return;
    }
    if (scalarTarget) {
      Append(body, "  %s = (%s).x;\n", target.c_str(), v.c_str());
      return;
    }
    if (mask == 0xF) {
      Append(body, "  %s = %s;\n", target.c_str(), v.c_str());
      return;
    }
    body += "  { let t = " + v + ";";
    static const char kComp[] = "xyzw";
    for (int c = 0; c < 4; ++c)
      if (mask & (1u << c)) Append(body, " %s.%c = t.%c;", target.c_str(), kComp[c], kComp[c]);
    body += " }\n";
  }

  // Matrix ops: dst.c = dot(src, row c) for `rows` consecutive registers.
  void Matrix(const Instr& ins, int rows, int width) {
    DWORD dst = ins.args[0];
    std::string parts[4];
    for (int r = 0; r < rows; ++r) {
      size_t p = 1;
      std::string a = Source(ins, p);
      std::string m = Source(ins, p, r);
      parts[r] = width == 4 ? "dot(" + a + ", " + m + ")" : "dot((" + a + ").xyz, (" + m + ").xyz)";
    }
    std::string v = "vec4f(";
    for (int c = 0; c < 4; ++c) v += (c ? ", " : "") + (c < rows ? parts[c] : std::string("0.0"));
    v += ")";
    Assign(dst, v, false);
  }

  bool Emit(const Instr& ins) {
    bool ps1 = !vertex && shader.major < 2;
    size_t p = 1;
    switch (ins.opcode) {
      case kNop:
      case kDcl:
      case kDef:
      case kPhase: return true;
      case kMov:
      case kMova: Assign(ins.args[0], Source(ins, p), ps1); return true;
      case kAdd: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], a + " + " + b, ps1);
        return true;
      }
      case kSub: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], a + " - " + b, ps1);
        return true;
      }
      case kMul: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], a + " * " + b, ps1);
        return true;
      }
      case kMad: {
        std::string a = Source(ins, p), b = Source(ins, p), c = Source(ins, p);
        Assign(ins.args[0], a + " * " + b + " + " + c, ps1);
        return true;
      }
      case kDp3: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], "vec4f(dot((" + a + ").xyz, (" + b + ").xyz))", ps1);
        return true;
      }
      case kDp4: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], "vec4f(dot(" + a + ", " + b + "))", ps1);
        return true;
      }
      case kRcp: Assign(ins.args[0], "vec4f(1.0 / " + Scalar(ins, p) + ")", false); return true;
      case kRsq: Assign(ins.args[0], "vec4f(inverseSqrt(abs(" + Scalar(ins, p) + ")))", false); return true;
      case kMin:
      case kMax: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], std::string(ins.opcode == kMin ? "min(" : "max(") + a + ", " + b + ")", ps1);
        return true;
      }
      case kSlt:
      case kSge: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], "select(vec4f(0.0), vec4f(1.0), " + a + (ins.opcode == kSlt ? " < " : " >= ") + b + ")",
               false);
        return true;
      }
      case kExp:
        Assign(ins.args[0], "vec4f(exp2(" + Scalar(ins, p) + "))", false);
        return true;
      case kExpp: {
        std::string x = Scalar(ins, p);
        if (shader.major >= 2)
          Assign(ins.args[0], "vec4f(exp2(" + x + "))", false);
        else
          Assign(ins.args[0], "vec4f(exp2(floor(" + x + ")), fract(" + x + "), exp2(" + x + "), 1.0)", false);
        return true;
      }
      case kLog:
      case kLogp: Assign(ins.args[0], "vec4f(log2(abs(" + Scalar(ins, p) + ")))", false); return true;
      case kLit: {
        std::string s = Source(ins, p);
        Assign(ins.args[0],
               "vec4f(1.0, max((" + s + ").x, 0.0), select(0.0, pow(max((" + s + ").y, 0.0), clamp((" + s +
                   ").w, -128.0, 128.0)), (" + s + ").x > 0.0), 1.0)",
               false);
        return true;
      }
      case kDst: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], "vec4f(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)", false);
        return true;
      }
      case kLrp: {
        std::string a = Source(ins, p), b = Source(ins, p), c = Source(ins, p);
        Assign(ins.args[0], "mix(" + c + ", " + b + ", " + a + ")", ps1);
        return true;
      }
      case kFrc: Assign(ins.args[0], "fract(" + Source(ins, p) + ")", false); return true;
      case kM4x4: Matrix(ins, 4, 4); return true;
      case kM4x3: Matrix(ins, 3, 4); return true;
      case kM3x4: Matrix(ins, 4, 3); return true;
      case kM3x3: Matrix(ins, 3, 3); return true;
      case kM3x2: Matrix(ins, 2, 3); return true;
      case kPow: {
        std::string a = Scalar(ins, p), b = Scalar(ins, p);
        Assign(ins.args[0], "vec4f(pow(abs(" + a + "), " + b + "))", false);
        return true;
      }
      case kAbs: Assign(ins.args[0], "abs(" + Source(ins, p) + ")", false); return true;
      case kNrm: {
        std::string s = Source(ins, p);
        Assign(ins.args[0], "vec4f(normalize((" + s + ").xyz), 1.0)", false);
        return true;
      }
      case kCrs: {
        std::string a = Source(ins, p), b = Source(ins, p);
        Assign(ins.args[0], "vec4f(cross((" + a + ").xyz, (" + b + ").xyz), 0.0)", false);
        return true;
      }
      case kSgn: Assign(ins.args[0], "sign(" + Source(ins, p) + ")", false); return true;
      case kCmp: {
        std::string a = Source(ins, p), b = Source(ins, p), c = Source(ins, p);
        Assign(ins.args[0], "select(" + c + ", " + b + ", " + a + " >= vec4f(0.0))", ps1);
        return true;
      }
      case kCnd: {
        std::string a = Source(ins, p), b = Source(ins, p), c = Source(ins, p);
        std::string cond = ps1 ? "vec4f((" + a + ").w)" : a;  // ps_1_x tests r0.a
        Assign(ins.args[0], "select(" + c + ", " + b + ", " + cond + " > vec4f(0.5))", ps1);
        return true;
      }
      case kTex: {
        if (shader.major >= 2) return false;  // texld (ps_2_0) is not used by the game
        uint32_t n = RegNum(ins.args[0]);
        textures.insert(n);
        Append(body, "  t%u = textureSample(t%uTex, s%u, in.uv%u.xy);\n", n, n, n, n);
        return true;
      }
      case kTexCoord: {
        uint32_t n = RegNum(ins.args[0]);
        textures.insert(n);
        Append(body, "  t%u = saturate(vec4f(in.uv%u.xyz, 1.0));\n", n, n);
        return true;
      }
      case kTexKill: {
        size_t q = 0;
        std::string r = Register(ins, q);
        body += "  if (any((" + r + ").xyz < vec3f(0.0))) { discard; }\n";
        return true;
      }
      default: return false;
    }
  }
};

void CollectDefs(const std::vector<Instr>& code, Emitter& e) {
  for (const Instr& ins : code) {
    if (ins.opcode != kDef || ins.args.size() < 5) continue;
    float f[4];
    std::memcpy(f, &ins.args[1], 16);
    char buf[160];
    snprintf(buf, sizeof(buf), "vec4f(%.9g, %.9g, %.9g, %.9g)", f[0], f[1], f[2], f[3]);
    e.defs[RegNum(ins.args[0])] = buf;
  }
}

}  // namespace

uint32_t RegisterShader(const DWORD* code, bool vertex) {
  size_t n = 1;
  while (code[n] != 0x0000FFFF) {
    DWORD op = code[n] & 0xFFFF;
    if (op == kComment) n += (code[n] >> 16) & 0x7FFF;
    ++n;
  }
  std::vector<DWORD> tokens(code, code + n + 1);
  auto it = g_ids.find(tokens);
  if (it != g_ids.end()) return it->second;

  ParsedShader s;
  s.vertex = vertex;
  s.major = static_cast<int>((tokens[0] >> 8) & 0xFF);
  s.minor = static_cast<int>(tokens[0] & 0xFF);
  s.tokens = tokens;
  std::vector<Instr> code2;
  s.ok = Parse(s, &code2);
  if (s.ok && vertex) {
    for (const Instr& ins : code2) {
      if (ins.opcode != kDcl || ins.args.size() < 2) continue;
      if (RegType(ins.args[1]) != kInput) continue;
      s.inputs.push_back({static_cast<uint8_t>(RegNum(ins.args[1])), static_cast<uint8_t>(ins.args[0] & 0x1F),
                          static_cast<uint8_t>((ins.args[0] >> 16) & 0xF)});
    }
    if (s.major < 2) s.ok = false, s.error = "vs_1_x input declarations are not translated";
  }
  g_shaders.push_back(std::move(s));
  uint32_t id = static_cast<uint32_t>(g_shaders.size());
  g_ids[tokens] = id;
  return id;
}

const ParsedShader* GetShader(uint32_t id) { return id && id <= g_shaders.size() ? &g_shaders[id - 1] : nullptr; }

std::string TranslateVertexShader(const ParsedShader& shader, const uint8_t* inputClass,
                                  const uint8_t* inputComponents) {
  std::vector<Instr> code;
  ParsedShader copy = shader;
  Parse(copy, &code);
  Emitter e{shader, true, {}, {}, {}, {}};
  CollectDefs(code, e);
  for (const Instr& ins : code)
    if (!e.Emit(ins)) Append(e.body, "  // unsupported opcode %u\n", ins.opcode);

  std::string s = "struct VIn {\n";
  bool any = false;
  for (const ShaderInput& in : shader.inputs) {
    if (!inputClass[in.reg]) continue;
    static const char* kF[] = {"", "f32", "vec2f", "vec3f", "vec4f"};
    static const char* kI[] = {"", "i32", "vec2i", "vec3i", "vec4i"};
    static const char* kU[] = {"", "u32", "vec2u", "vec3u", "vec4u"};
    int n = inputComponents[in.reg];
    const char* type = inputClass[in.reg] == kInputSint ? kI[n] : inputClass[in.reg] == kInputUint ? kU[n] : kF[n];
    Append(s, "  @location(%u) v%u: %s,\n", in.reg, in.reg, type);
    any = true;
  }
  if (!any) s += "  @builtin(vertex_index) unused: u32,\n";
  s += "};\n\n@vertex fn vs(in: VIn) -> VOut {\n  var out: VOut;\n"
       "  out.diffuse = vec4f(1.0);\n  out.specular = vec4f(0.0);\n"
       "  out.uv0 = vec4f(0.0, 0.0, 0.0, 1.0);\n  out.uv1 = out.uv0;\n  out.uv2 = out.uv0;\n  out.uv3 = out.uv0;\n"
       "  out.fogFactor = 1.0;\n  var a0 = vec4i(0);\n";
  for (const ShaderInput& in : shader.inputs) {
    if (!inputClass[in.reg]) {
      Append(s, "  let v%u = vec4f(0.0, 0.0, 0.0, 1.0);\n", in.reg);
      continue;
    }
    int n = inputComponents[in.reg];
    std::string v = inputClass[in.reg] == kInputFloat ? "in.v" + std::to_string(in.reg)
                                                     : "vec" + std::to_string(n) + "f(in.v" + std::to_string(in.reg) + ")";
    if (n == 1) v = "vec4f(" + v + ", 0.0, 0.0, 1.0)";
    else if (n == 2) v = "vec4f(" + v + ", 0.0, 1.0)";
    else if (n == 3) v = "vec4f(" + v + ", 1.0)";
    Append(s, "  let v%u = %s;\n", in.reg, v.c_str());
  }
  for (uint32_t r : e.temps) Append(s, "  var r%u = vec4f(0.0);\n", r);
  s += e.body;
  s += "  out.fogDepth = out.pos.w;\n  return out;\n}\n\n";
  return s;
}

std::string TranslatePixelShader(const ParsedShader& shader) {
  std::vector<Instr> code;
  ParsedShader copy = shader;
  Parse(copy, &code);
  Emitter e{shader, false, {}, {}, {}, {}};
  CollectDefs(code, e);
  for (const Instr& ins : code)
    if (!e.Emit(ins)) Append(e.body, "  // unsupported opcode %u\n", ins.opcode);
  std::string s;
  // Texture sampling must stay in uniform control flow: sample up front.
  for (uint32_t t : e.textures) Append(s, "  var t%u = vec4f(0.0);\n", t);
  for (uint32_t r : e.temps)
    if (r != 0) Append(s, "  var r%u = vec4f(0.0);\n", r);
  s += "  var r0 = vec4f(0.0);\n";
  s += e.body;
  s += shader.major < 2 ? "  var cur = r0;\n" : "";
  return s;
}

}  // namespace agpu::wgb
