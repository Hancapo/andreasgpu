#include "webgpu/atmosphere.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace agpu::wgb::atmosphere {
namespace {

constexpr uint32_t kTransmittanceW = 256, kTransmittanceH = 64;
constexpr uint32_t kMultiScatteringSize = 32;
constexpr uint32_t kSkyViewW = 192, kSkyViewH = 108;
constexpr uint32_t kApSize = 32;

// Layout of the WGSL `A` uniform.
struct Uniforms {
  float invViewProj[16];
  float camPos[4];  // xyz camera (m), w exposure
  float sunDir[4];  // xyz towards the sun, w sun illuminance
  float params[4];  // x day factor, y aerial perspective distance (m), z cos(sun disk radius)
  float size[4];    // full-resolution width, height
};

}  // namespace

// Shared by every atmosphere pass and by the clouds. Lengths in km inside the
// atmosphere (squares of 6360000 m lose the low-altitude precision in f32).
const char* CommonWgsl() {
  return R"(
const kPi = 3.14159265;
const kBottom = 6360.0;  // km
const kTop = 6460.0;
const kRayleighScattering = vec3f(5.802, 13.558, 33.1) * 1e-3;  // 1/km
const kRayleighH = 8.0;
const kMieScattering = 3.996e-3;
const kMieExtinction = 4.44e-3;  // Hillaire 2020 (absorption 10% of scattering)
const kMieH = 1.2;
const kMieG = 0.8;
const kOzoneAbsorption = vec3f(0.650, 1.881, 0.085) * 1e-3;
const kOzoneCenter = 25.0;
const kOzoneWidth = 15.0;
const kGroundAlbedo = vec3f(0.3);

struct Medium { scattering: vec3f, extinction: vec3f, rayleigh: vec3f, mie: vec3f };
fn sampleMedium(h: f32) -> Medium {
  let hh = max(h, 0.0);
  let dR = exp(-hh / kRayleighH);
  let dM = exp(-hh / kMieH);
  let dO = max(0.0, 1.0 - abs(hh - kOzoneCenter) / kOzoneWidth);
  var m: Medium;
  m.rayleigh = kRayleighScattering * dR;
  m.mie = vec3f(kMieScattering * dM);
  m.scattering = m.rayleigh + m.mie;
  m.extinction = m.rayleigh + vec3f(kMieExtinction * dM) + kOzoneAbsorption * dO;
  return m;
}
fn rayleighPhase(c: f32) -> f32 { return 3.0 / (16.0 * kPi) * (1.0 + c * c); }
fn miePhase(c: f32, g: f32) -> f32 {
  let k = 3.0 / (8.0 * kPi) * (1.0 - g * g) / (2.0 + g * g);
  return k * (1.0 + c * c) / pow(max(0.0001, 1.0 + g * g - 2.0 * g * c), 1.5);
}
// Ray against a sphere at the origin: (near, far), or (-1, -1) for a miss.
fn atmoRaySphere(o: vec3f, d: vec3f, r: f32) -> vec2f {
  let b = dot(o, d);
  let c = dot(o, o) - r * r;
  let disc = b * b - c;
  if (disc < 0.0) { return vec2f(-1.0); }
  let s = sqrt(disc);
  return vec2f(-b - s, -b + s);
}
// World position (m, Z up) to planet-centred km.
fn planetPos(w: vec3f) -> vec3f { return vec3f(w.xy * 0.001, kBottom + w.z * 0.001); }

// Transmittance LUT (Bruneton/Hillaire horizon-aware mapping).
fn transmittanceUv(r: f32, mu: f32) -> vec2f {
  let H = sqrt(kTop * kTop - kBottom * kBottom);
  let rho = sqrt(max(0.0, r * r - kBottom * kBottom));
  let disc = r * r * (mu * mu - 1.0) + kTop * kTop;
  let d = max(0.0, -r * mu + sqrt(max(0.0, disc)));
  let dMin = kTop - r;
  let dMax = rho + H;
  return vec2f((d - dMin) / (dMax - dMin), rho / H);
}
fn atmoTransmittance(lut: texture_2d<f32>, smp: sampler, r: f32, mu: f32) -> vec3f {
  return textureSampleLevel(lut, smp, transmittanceUv(r, mu), 0.0).rgb;
}
fn multiScatteringUv(r: f32, muS: f32) -> vec2f {
  return vec2f(0.5 + 0.5 * muS, saturate((r - kBottom) / (kTop - kBottom)));
}

// Sky-view LUT: azimuth on u, horizon-compressed elevation on v (horizon at 0.5).
fn skyViewUv(r: f32, viewZenithCos: f32, azimuth: f32) -> vec2f {
  let rho = sqrt(max(0.0, r * r - kBottom * kBottom));
  let beta = acos(clamp(rho / r, -1.0, 1.0));
  let zenithHorizon = kPi - beta;
  let theta = acos(clamp(viewZenithCos, -1.0, 1.0));
  var v = 0.5;
  if (theta < zenithHorizon) {
    v = 0.5 + 0.5 * sqrt(saturate(1.0 - theta / zenithHorizon));
  } else {
    v = 0.5 - 0.5 * sqrt(saturate((theta - zenithHorizon) / beta));
  }
  return vec2f(fract(azimuth / (2.0 * kPi)), v);
}
fn sampleSkyView(sky: texture_2d<f32>, smp: sampler, r: f32, dir: vec3f) -> vec3f {
  let uv = skyViewUv(r, dir.z, atan2(dir.y, dir.x));
  return textureSampleLevel(sky, smp, uv, 0.0).rgb;
}

// Spec section 4: the bridge to the clouds.
fn sampleSkyAmbientIrradiance(sky: texture_2d<f32>, ms: texture_2d<f32>, smp: sampler, r: f32, sunDir: vec3f,
                              sunIllum: f32) -> vec3f {
  let multi = textureSampleLevel(ms, smp, multiScatteringUv(r, sunDir.z), 0.0).rgb;
  let zenith = textureSampleLevel(sky, smp, vec2f(0.5, 0.85), 0.0).rgb;
  return zenith * 0.5 + multi * sunIllum * 0.25;
}
// Aerial perspective: slices end at ((z + 1) / 32)^2 * maxDist.
fn sampleAerialPerspective(ap: texture_3d<f32>, smp: sampler, uv: vec2f, distM: f32, maxDistM: f32) -> vec4f {
  let w = sqrt(saturate(distM / maxDistM));
  let weight = saturate(w * 32.0);
  let a = textureSampleLevel(ap, smp, vec3f(uv, max(w - 0.5 / 32.0, 0.0)), 0.0);
  return vec4f(a.rgb * weight, mix(1.0, a.a, weight));
}
)";
}

namespace {

const char* kUniformsWgsl = R"(
struct A {
  invViewProj: mat4x4f,
  camPos: vec4f,
  sunDir: vec4f,
  params: vec4f,
  size: vec4f,
};
@group(0) @binding(0) var<uniform> a: A;

fn rayDirOf(uv: vec2f) -> vec3f {
  let ndc = vec2f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  let p0 = a.invViewProj * vec4f(ndc, 0.0, 1.0);
  let p1 = a.invViewProj * vec4f(ndc, 1.0, 1.0);
  return normalize(p1.xyz / p1.w - p0.xyz / p0.w);
}
)";

// Static LUTs (no uniforms).
const char* kStaticWgsl = R"(
@group(0) @binding(0) var transmittanceOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(1) var multiOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var transmittanceTex: texture_2d<f32>;
@group(0) @binding(3) var clampSmp: sampler;

@compute @workgroup_size(8, 8)
fn genTransmittance(@builtin(global_invocation_id) id: vec3u) {
  let uv = (vec2f(id.xy) + 0.5) / vec2f(256.0, 64.0);
  let H = sqrt(kTop * kTop - kBottom * kBottom);
  let rho = H * uv.y;
  let r = sqrt(rho * rho + kBottom * kBottom);
  let dMin = kTop - r;
  let dMax = rho + H;
  let d = dMin + uv.x * (dMax - dMin);
  var mu = 1.0;
  if (d > 0.0) { mu = clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0); }
  let o = vec3f(0.0, 0.0, r);
  let dir = vec3f(sqrt(max(0.0, 1.0 - mu * mu)), 0.0, mu);
  let steps = 40;
  let dt = d / f32(steps);
  var optical = vec3f(0.0);
  for (var i = 0; i < steps; i++) {
    let p = o + dir * (f32(i) + 0.5) * dt;
    optical += sampleMedium(length(p) - kBottom).extinction * dt;
  }
  textureStore(transmittanceOut, id.xy, vec4f(exp(-optical), 1.0));
}

// Hillaire 2020 multiple scattering: second-order light over 64 directions and the
// transfer factor f_ms, summed as the geometric series L2 / (1 - f_ms).
@compute @workgroup_size(8, 8)
fn genMultiScattering(@builtin(global_invocation_id) id: vec3u) {
  let uv = (vec2f(id.xy) + 0.5) / 32.0;
  let muS = uv.x * 2.0 - 1.0;
  let r = kBottom + 0.01 + uv.y * (kTop - kBottom - 0.02);
  let sun = vec3f(sqrt(max(0.0, 1.0 - muS * muS)), 0.0, muS);
  let o = vec3f(0.0, 0.0, r);
  let iso = 1.0 / (4.0 * kPi);
  var l2 = vec3f(0.0);
  var fms = vec3f(0.0);
  for (var i = 0; i < 8; i++) {
    for (var j = 0; j < 8; j++) {
      let theta = 2.0 * kPi * (f32(i) + 0.5) / 8.0;
      let cosPhi = 1.0 - 2.0 * (f32(j) + 0.5) / 8.0;
      let sinPhi = sqrt(max(0.0, 1.0 - cosPhi * cosPhi));
      let d = vec3f(cos(theta) * sinPhi, sin(theta) * sinPhi, cosPhi);
      let ground = atmoRaySphere(o, d, kBottom);
      let top = atmoRaySphere(o, d, kTop);
      let hitsGround = ground.x > 0.0;
      let tMax = select(top.y, ground.x, hitsGround);
      let steps = 20;
      let dt = tMax / f32(steps);
      var L = vec3f(0.0);
      var ms = vec3f(0.0);
      var throughput = vec3f(1.0);
      for (var k = 0; k < steps; k++) {
        let p = o + d * (f32(k) + 0.5) * dt;
        let pr = length(p);
        let m = sampleMedium(pr - kBottom);
        let stepT = exp(-m.extinction * dt);
        let up = p / pr;
        let sunT = atmoTransmittance(transmittanceTex, clampSmp, pr, dot(up, sun));
        let shadow = select(1.0, 0.0, atmoRaySphere(p, sun, kBottom).x > 0.0);
        let S = m.scattering * iso * sunT * shadow;
        let ext = max(m.extinction, vec3f(1e-7));
        L += throughput * (S - S * stepT) / ext;
        ms += throughput * (m.scattering - m.scattering * stepT) / ext;
        throughput *= stepT;
      }
      if (hitsGround) {
        let gp = o + d * ground.x;
        let n = normalize(gp);
        let sunT = atmoTransmittance(transmittanceTex, clampSmp, length(gp), dot(n, sun));
        L += throughput * sunT * max(0.0, dot(n, sun)) * kGroundAlbedo / kPi;
      }
      l2 += L / 64.0;
      fms += ms / 64.0;
    }
  }
  textureStore(multiOut, id.xy, vec4f(l2 / (1.0 - fms), 1.0));
}
)";

// Per-frame LUTs.
const char* kFrameWgsl = R"(
@group(0) @binding(1) var transmittanceTex: texture_2d<f32>;
@group(0) @binding(2) var multiTex: texture_2d<f32>;
@group(0) @binding(3) var clampSmp: sampler;
@group(0) @binding(4) var skyOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var apOut: texture_storage_3d<rgba16float, write>;

// In-scattered light and transmittance along a ray segment [t0, t1] (km).
struct Segment { L: vec3f, throughput: vec3f };
fn integrate(o: vec3f, d: vec3f, t0: f32, t1: f32, steps: i32, start: Segment) -> Segment {
  var s = start;
  let sun = a.sunDir.xyz;
  let cosTheta = dot(d, sun);
  let phaseR = rayleighPhase(cosTheta);
  let phaseM = miePhase(cosTheta, kMieG);
  let dt = (t1 - t0) / f32(steps);
  for (var i = 0; i < steps; i++) {
    let p = o + d * (t0 + (f32(i) + 0.5) * dt);
    let pr = length(p);
    let up = p / pr;
    let m = sampleMedium(pr - kBottom);
    let stepT = exp(-m.extinction * dt);
    let muS = dot(up, sun);
    let sunT = atmoTransmittance(transmittanceTex, clampSmp, pr, muS);
    let shadow = select(1.0, 0.0, atmoRaySphere(p, sun, kBottom).x > 0.0);
    let psi = textureSampleLevel(multiTex, clampSmp, multiScatteringUv(pr, muS), 0.0).rgb;
    let S = a.sunDir.w * (shadow * sunT * (m.rayleigh * phaseR + m.mie * phaseM) + psi * m.scattering);
    let ext = max(m.extinction, vec3f(1e-7));
    s.L += s.throughput * (S - S * stepT) / ext;
    s.throughput *= stepT;
  }
  return s;
}
fn atmosphereExit(o: vec3f, d: vec3f) -> f32 {
  let ground = atmoRaySphere(o, d, kBottom);
  if (ground.x > 0.0) { return ground.x; }
  return max(atmoRaySphere(o, d, kTop).y, 0.0);
}

@compute @workgroup_size(8, 8)
fn genSkyView(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= 192u || id.y >= 108u) { return; }
  let uv = (vec2f(id.xy) + 0.5) / vec2f(192.0, 108.0);
  let r = max(planetPos(a.camPos.xyz).z, kBottom + 0.001);
  let rho = sqrt(max(0.0, r * r - kBottom * kBottom));
  let beta = acos(clamp(rho / r, -1.0, 1.0));
  let zenithHorizon = kPi - beta;
  let coord = select(2.0 * uv.y - 1.0, 1.0 - 2.0 * uv.y, uv.y < 0.5);
  let adjV = coord * coord;
  let theta = select(zenithHorizon * (1.0 - adjV), zenithHorizon + adjV * beta, uv.y < 0.5);
  let phi = uv.x * 2.0 * kPi;
  let d = vec3f(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
  let o = vec3f(0.0, 0.0, r);
  var s: Segment;
  s.L = vec3f(0.0);
  s.throughput = vec3f(1.0);
  s = integrate(o, d, 0.0, atmosphereExit(o, d), 32, s);
  textureStore(skyOut, id.xy, vec4f(s.L, 1.0));
}

@compute @workgroup_size(8, 8)
fn genAerialPerspective(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= 32u || id.y >= 32u) { return; }
  let uv = (vec2f(id.xy) + 0.5) / 32.0;
  let d = rayDirOf(uv);
  let o = planetPos(a.camPos.xyz);
  let maxKm = a.params.y * 0.001;
  let exitKm = atmosphereExit(o, d);
  var s: Segment;
  s.L = vec3f(0.0);
  s.throughput = vec3f(1.0);
  var t = 0.0;
  for (var z = 0u; z < 32u; z++) {
    let w = f32(z + 1u) / 32.0;
    let t1 = min(w * w * maxKm, exitKm);
    if (t1 > t) {
      s = integrate(o, d, t, t1, 2, s);
      t = t1;
    }
    textureStore(apOut, vec3u(id.xy, z), vec4f(s.L, dot(s.throughput, vec3f(1.0 / 3.0))));
  }
}
)";

// Sky and aerial perspective over the scene. Output is premultiplied:
// result = src + dst * (1 - src.a).
const char* kApplyWgsl = R"(
@group(0) @binding(1) var skyTex: texture_2d<f32>;
@group(0) @binding(2) var transmittanceTex: texture_2d<f32>;
@group(0) @binding(3) var apTex: texture_3d<f32>;
@group(0) @binding(4) var clampSmp: sampler;
@group(0) @binding(5) var repeatSmp: sampler;
@group(0) @binding(6) var depthTex: texture_depth_2d;

struct V { @builtin(position) pos: vec4f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> V {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;
  return V(vec4f(p, 0.0, 1.0));
}
fn unprojectAt(uv: vec2f, z: f32) -> vec3f {
  let p = a.invViewProj * vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
  return p.xyz / p.w;
}
@fragment fn fs(v: V) -> @location(0) vec4f {
  let p = vec2i(v.pos.xy);
  let uv = v.pos.xy / a.size.xy;
  let depth = textureLoad(depthTex, p, 0);
  let dir = rayDirOf(uv);
  let day = a.params.x;
  let exposure = a.camPos.w;
  // Sky: cleared depth, or the far plane where the game draws its sky.
  let dist = length(unprojectAt(uv, depth) - a.camPos.xyz);
  let far = length(unprojectAt(uv, 1.0) - a.camPos.xyz);
  if (depth >= 1.0 || dist > far * 0.97) {
    let r = max(planetPos(a.camPos.xyz).z, kBottom + 0.001);
    var L = sampleSkyView(skyTex, repeatSmp, r, dir);
    let sunCos = dot(dir, a.sunDir.xyz);
    let disk = smoothstep(a.params.z - 0.00002, a.params.z, sunCos);
    L += disk * a.sunDir.w * 40.0 * atmoTransmittance(transmittanceTex, clampSmp, r, dir.z);
    return vec4f((1.0 - exp(-L * exposure)) * day, day);
  }
  let ap = sampleAerialPerspective(apTex, clampSmp, uv, dist, a.params.y);
  return vec4f(ap.rgb * exposure * day, (1.0 - ap.a) * day);
}
)";

struct State {
  wgpu::Texture transmittance, multi, sky, ap;
  Views views;
  wgpu::Buffer uniforms;
  wgpu::Sampler clampSampler, repeatSampler;
  wgpu::BindGroupLayout frameLayout, applyLayout;
  wgpu::ComputePipeline skyPipeline, apPipeline;
  wgpu::RenderPipeline apply;
  wgpu::BindGroup frameGroup;
} *g_state = nullptr;

wgpu::ShaderModule Module(const wgpu::Device& device, const std::string& code) {
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor desc{};
  desc.nextInChain = &wgsl;
  return device.CreateShaderModule(&desc);
}
wgpu::Texture MakeTexture(const wgpu::Device& device, uint32_t w, uint32_t h, uint32_t d,
                          wgpu::TextureDimension dim = wgpu::TextureDimension::e2D) {
  wgpu::TextureDescriptor td{};
  td.size = {w, h, d};
  td.format = wgpu::TextureFormat::RGBA16Float;
  td.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding;
  td.dimension = dim;
  return device.CreateTexture(&td);
}
wgpu::BindGroupLayoutEntry Entry(uint32_t binding, wgpu::ShaderStage stages) {
  wgpu::BindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = stages;
  return e;
}
wgpu::BindGroupLayoutEntry UniformEntry(uint32_t b, wgpu::ShaderStage s) {
  auto e = Entry(b, s);
  e.buffer.type = wgpu::BufferBindingType::Uniform;
  e.buffer.minBindingSize = sizeof(Uniforms);
  return e;
}
wgpu::BindGroupLayoutEntry TextureEntry(uint32_t b, wgpu::ShaderStage s,
                                        wgpu::TextureViewDimension dim = wgpu::TextureViewDimension::e2D,
                                        wgpu::TextureSampleType type = wgpu::TextureSampleType::Float) {
  auto e = Entry(b, s);
  e.texture.sampleType = type;
  e.texture.viewDimension = dim;
  return e;
}
wgpu::BindGroupLayoutEntry StorageEntry(uint32_t b, wgpu::TextureViewDimension dim = wgpu::TextureViewDimension::e2D) {
  auto e = Entry(b, wgpu::ShaderStage::Compute);
  e.storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  e.storageTexture.format = wgpu::TextureFormat::RGBA16Float;
  e.storageTexture.viewDimension = dim;
  return e;
}
wgpu::BindGroupLayoutEntry SamplerEntry(uint32_t b, wgpu::ShaderStage s) {
  auto e = Entry(b, s);
  e.sampler.type = wgpu::SamplerBindingType::Filtering;
  return e;
}
wgpu::BindGroupLayout Layout(const wgpu::Device& device, const std::vector<wgpu::BindGroupLayoutEntry>& entries) {
  wgpu::BindGroupLayoutDescriptor d{};
  d.entryCount = entries.size();
  d.entries = entries.data();
  return device.CreateBindGroupLayout(&d);
}
wgpu::PipelineLayout PipelineLayoutOf(const wgpu::Device& device, const wgpu::BindGroupLayout& layout) {
  wgpu::PipelineLayoutDescriptor d{};
  d.bindGroupLayoutCount = 1;
  d.bindGroupLayouts = &layout;
  return device.CreatePipelineLayout(&d);
}
wgpu::ComputePipeline Compute(const wgpu::Device& device, const wgpu::ShaderModule& module, const char* entry,
                              const wgpu::BindGroupLayout& layout) {
  wgpu::ComputePipelineDescriptor d{};
  d.layout = PipelineLayoutOf(device, layout);
  d.compute.module = module;
  d.compute.entryPoint = entry;
  return device.CreateComputePipeline(&d);
}
wgpu::BindGroup Group(const wgpu::Device& device, const wgpu::BindGroupLayout& layout,
                      std::vector<wgpu::BindGroupEntry> entries) {
  for (size_t i = 0; i < entries.size(); ++i) entries[i].binding = static_cast<uint32_t>(i);
  wgpu::BindGroupDescriptor d{};
  d.layout = layout;
  d.entryCount = entries.size();
  d.entries = entries.data();
  return device.CreateBindGroup(&d);
}
wgpu::BindGroupEntry ViewEntry(const wgpu::TextureView& v) {
  wgpu::BindGroupEntry e{};
  e.textureView = v;
  return e;
}
wgpu::BindGroupEntry SamplerOf(const wgpu::Sampler& s) {
  wgpu::BindGroupEntry e{};
  e.sampler = s;
  return e;
}
wgpu::BindGroupEntry BufferEntry(const wgpu::Buffer& b) {
  wgpu::BindGroupEntry e{};
  e.buffer = b;
  e.size = sizeof(Uniforms);
  return e;
}

}  // namespace

bool Init(const wgpu::Device& device, const wgpu::Queue& queue, wgpu::TextureFormat target) {
  if (g_state) return true;
  auto* s = new State();
  g_state = s;
  const auto compute = wgpu::ShaderStage::Compute;
  const auto fragment = wgpu::ShaderStage::Fragment;
  s->transmittance = MakeTexture(device, kTransmittanceW, kTransmittanceH, 1);
  s->multi = MakeTexture(device, kMultiScatteringSize, kMultiScatteringSize, 1);
  s->sky = MakeTexture(device, kSkyViewW, kSkyViewH, 1);
  s->ap = MakeTexture(device, kApSize, kApSize, kApSize, wgpu::TextureDimension::e3D);
  s->views = {s->transmittance.CreateView(), s->multi.CreateView(), s->sky.CreateView(), s->ap.CreateView()};

  wgpu::SamplerDescriptor sd{};
  sd.magFilter = sd.minFilter = wgpu::FilterMode::Linear;
  sd.addressModeU = sd.addressModeV = sd.addressModeW = wgpu::AddressMode::ClampToEdge;
  s->clampSampler = device.CreateSampler(&sd);
  sd.addressModeU = wgpu::AddressMode::Repeat;  // sky-view azimuth wraps
  s->repeatSampler = device.CreateSampler(&sd);
  wgpu::BufferDescriptor ub{};
  ub.size = sizeof(Uniforms);
  ub.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  s->uniforms = device.CreateBuffer(&ub);

  // Static LUTs: transmittance, then multiple scattering (reads it).
  {
    wgpu::BindGroupLayout layout = Layout(device, {StorageEntry(0), StorageEntry(1), TextureEntry(2, compute),
                                                   SamplerEntry(3, compute)});
    wgpu::ShaderModule module = Module(device, std::string(CommonWgsl()) + kStaticWgsl);
    // The transmittance pass binds a dummy as its own input (it does not read it).
    wgpu::Texture dummy = MakeTexture(device, 1, 1, 1);
    wgpu::TextureView transmittanceStore = s->transmittance.CreateView();
    wgpu::BindGroup first = Group(device, layout, {ViewEntry(transmittanceStore), ViewEntry(s->multi.CreateView()),
                                                   ViewEntry(dummy.CreateView()), SamplerOf(s->clampSampler)});
    wgpu::Texture dummyOut = MakeTexture(device, 1, 1, 1);
    wgpu::BindGroup second = Group(device, layout, {ViewEntry(dummyOut.CreateView()), ViewEntry(s->multi.CreateView()),
                                                    ViewEntry(s->views.transmittance), SamplerOf(s->clampSampler)});
    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(Compute(device, module, "genTransmittance", layout));
    pass.SetBindGroup(0, first);
    pass.DispatchWorkgroups(kTransmittanceW / 8, kTransmittanceH / 8, 1);
    pass.End();
    pass = encoder.BeginComputePass();
    pass.SetPipeline(Compute(device, module, "genMultiScattering", layout));
    pass.SetBindGroup(0, second);
    pass.DispatchWorkgroups(kMultiScatteringSize / 8, kMultiScatteringSize / 8, 1);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    queue.Submit(1, &commands);
  }

  // Per-frame LUTs.
  s->frameLayout = Layout(device, {UniformEntry(0, compute), TextureEntry(1, compute), TextureEntry(2, compute),
                                   SamplerEntry(3, compute), StorageEntry(4),
                                   StorageEntry(5, wgpu::TextureViewDimension::e3D)});
  wgpu::ShaderModule frameModule = Module(device, std::string(CommonWgsl()) + kUniformsWgsl + kFrameWgsl);
  s->skyPipeline = Compute(device, frameModule, "genSkyView", s->frameLayout);
  s->apPipeline = Compute(device, frameModule, "genAerialPerspective", s->frameLayout);
  s->frameGroup = Group(device, s->frameLayout,
                        {BufferEntry(s->uniforms), ViewEntry(s->views.transmittance), ViewEntry(s->views.multiScattering),
                         SamplerOf(s->clampSampler), ViewEntry(s->views.skyView), ViewEntry(s->views.aerialPerspective)});

  // Sky + aerial perspective over the scene.
  s->applyLayout = Layout(device, {UniformEntry(0, fragment), TextureEntry(1, fragment), TextureEntry(2, fragment),
                                   TextureEntry(3, fragment, wgpu::TextureViewDimension::e3D),
                                   SamplerEntry(4, fragment), SamplerEntry(5, fragment),
                                   TextureEntry(6, fragment, wgpu::TextureViewDimension::e2D,
                                                wgpu::TextureSampleType::Depth)});
  wgpu::ShaderModule applyModule = Module(device, std::string(CommonWgsl()) + kUniformsWgsl + kApplyWgsl);
  wgpu::BlendState blend{};
  blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha};
  blend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::Zero, wgpu::BlendFactor::One};
  wgpu::ColorTargetState color{};
  color.format = target;
  color.blend = &blend;
  wgpu::FragmentState fs{};
  fs.module = applyModule;
  fs.entryPoint = "fs";
  fs.targetCount = 1;
  fs.targets = &color;
  wgpu::RenderPipelineDescriptor rp{};
  rp.layout = PipelineLayoutOf(device, s->applyLayout);
  rp.vertex.module = applyModule;
  rp.vertex.entryPoint = "vs";
  rp.fragment = &fs;
  s->apply = device.CreateRenderPipeline(&rp);
  return true;
}

void Shutdown() {
  delete g_state;
  g_state = nullptr;
}

const Views& GetViews() { return g_state->views; }

float DayFactor(const float sun[3]) {
  float t = std::clamp((sun[2] + 0.12f) / 0.14f, 0.0f, 1.0f);  // sun from 7 deg below to 1 deg above the horizon
  return t * t * (3 - 2 * t);
}

void Update(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::CommandEncoder& encoder,
            const Frame& f, uint32_t width, uint32_t height) {
  (void)device;
  State* s = g_state;
  if (!s) return;
  Uniforms u{};
  std::memcpy(u.invViewProj, f.invViewProj, sizeof(u.invViewProj));
  std::memcpy(u.camPos, f.camera, sizeof(f.camera));
  u.camPos[3] = kExposure;
  float sun[3] = {f.game.sun[0], f.game.sun[1], f.game.sun[2]};
  if (sun[0] == 0 && sun[1] == 0 && sun[2] == 0) sun[2] = 1;
  std::memcpy(u.sunDir, sun, sizeof(sun));
  u.sunDir[3] = kSunIlluminance;
  u.params[0] = DayFactor(sun);
  u.params[1] = kAerialPerspectiveDistance;
  u.params[2] = std::cos(0.2665f * 3.14159265f / 180.0f);  // angular radius of the sun
  u.size[0] = static_cast<float>(width);
  u.size[1] = static_cast<float>(height);
  queue.WriteBuffer(s->uniforms, 0, &u, sizeof(u));
  wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
  pass.SetBindGroup(0, s->frameGroup);
  pass.SetPipeline(s->skyPipeline);
  pass.DispatchWorkgroups((kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8, 1);
  pass.SetPipeline(s->apPipeline);
  pass.DispatchWorkgroups(kApSize / 8, kApSize / 8, 1);
  pass.End();
}

void Apply(const wgpu::Device& device, const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
           const wgpu::TextureView& sceneDepth) {
  State* s = g_state;
  if (!s) return;
  wgpu::BindGroup group = Group(device, s->applyLayout,
                                {BufferEntry(s->uniforms), ViewEntry(s->views.skyView),
                                 ViewEntry(s->views.transmittance), ViewEntry(s->views.aerialPerspective),
                                 SamplerOf(s->clampSampler), SamplerOf(s->repeatSampler), ViewEntry(sceneDepth)});
  wgpu::RenderPassColorAttachment color{};
  color.view = target;
  color.loadOp = wgpu::LoadOp::Load;
  color.storeOp = wgpu::StoreOp::Store;
  wgpu::RenderPassDescriptor pd{};
  pd.colorAttachmentCount = 1;
  pd.colorAttachments = &color;
  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pd);
  pass.SetPipeline(s->apply);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}

}  // namespace agpu::wgb::atmosphere
