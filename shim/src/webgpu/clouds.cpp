#include "webgpu/clouds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <random>
#include <vector>

#include "log.h"
#include "webgpu/atmosphere.h"

namespace agpu::wgb::clouds {
namespace {

// ---------------------------------------------------------------------------
// Parameters (spec defaults where it gives them).

constexpr float kPlanetRadius = 6360000.0f;
// Spec defaults are 1500-4500 m, 1/3333 and 1/333; at GTA's scale (a 6 km map, fog
// at ~1 km) that gives a uniform field of small blobs, so: a lower, thinner layer,
// larger shapes, and a weather map that varies within the view (see kWeatherScale).
constexpr float kCloudBottom = 1200.0f;  // altitude (m)
constexpr float kCloudTop = 3600.0f;
constexpr float kShapeScale = 1.0f / 5000.0f;
constexpr float kDetailScale = 1.0f / 700.0f;
constexpr float kExtinction = 0.035f;  // spec 0.05 is too dark with this density range
constexpr float kMaxDistance = 60000.0f;
constexpr uint32_t kShapeSize = 128, kDetailSize = 32, kWeatherSize = 1024, kBlueSize = 64;

// Layout of the WGSL `U` uniform (every member vec4-aligned).
struct Uniforms {
  float invViewProj[16];
  float prevViewProj[16];
  float camPos[4];          // xyz camera (m), w planet radius
  float sunDir[4];          // xyz towards the light, w cloud bottom altitude
  float sunIllum[4];        // rgb light illuminance, w cloud top altitude
  float windDir[4];         // xyz normalized wind, w wind speed (m/s)
  float windOffset[4];      // xyz accumulated wind offset (m)
  float prevWindOffset[4];  // last frame's
  float params0[4];         // shape scale, detail scale, extinction, global coverage
  float params1[4];         // anvil amount, time (s), frame index, max distance (m)
  float ambient[4];         // rgb sky ambient, w cloud type bias (0 stratus .. 1 cumulus)
  float haze[4];            // rgb haze colour, w haze distance (m)
  float size[4];            // half-res w, h, full-res w, h
  float misc[4];            // x exposure, y history valid, z precipitation, w unused
  float atmo[4];            // xyz direction to the sun, w atmosphere weight (day factor, 0 = off)
};

// ---------------------------------------------------------------------------
// WGSL

const char* kCommon = R"(
struct U {
  invViewProj: mat4x4f,
  prevViewProj: mat4x4f,
  camPos: vec4f,
  sunDir: vec4f,
  sunIllum: vec4f,
  windDir: vec4f,
  windOffset: vec4f,
  prevWindOffset: vec4f,
  params0: vec4f,
  params1: vec4f,
  ambient: vec4f,
  haze: vec4f,
  size: vec4f,
  misc: vec4f,
  atmo: vec4f,
};
@group(0) @binding(0) var<uniform> u: U;

fn ndcOf(uv: vec2f) -> vec2f { return vec2f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }
fn unproject(uv: vec2f, z: f32) -> vec3f {
  let p = u.invViewProj * vec4f(ndcOf(uv), z, 1.0);
  return p.xyz / p.w;
}
fn rayDir(uv: vec2f) -> vec3f { return normalize(unproject(uv, 1.0) - unproject(uv, 0.0)); }
// Distance to the scene surface at `depth`; 1e9 for the sky (cleared depth, or the
// far plane where the game draws its sky).
fn sceneDistance(uv: vec2f, depth: f32) -> f32 {
  if (depth >= 1.0) { return 1e9; }
  let d = length(unproject(uv, depth) - u.camPos.xyz);
  let far = length(unproject(uv, 1.0) - u.camPos.xyz);
  return select(d, 1e9, d > far * 0.97);
}
)";

const char* kGenerate = R"(
@group(0) @binding(0) var shapeOut: texture_storage_3d<rgba8unorm, write>;
@group(0) @binding(1) var detailOut: texture_storage_3d<rgba8unorm, write>;
@group(0) @binding(2) var weatherOut: texture_storage_2d<rgba8unorm, write>;

fn pcg3d(v0: vec3u) -> vec3u {
  var v = v0 * 1664525u + 1013904223u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> vec3u(16u);
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return v;
}
fn rand3(c: vec3i, seed: u32) -> vec3f {
  return vec3f(pcg3d(vec3u(c) + vec3u(seed, seed * 7u, seed * 13u))) / 4294967295.0;
}
fn wrap(c: vec3i, period: i32) -> vec3i { return ((c % period) + period) % period; }

// Inverted cellular (Worley) noise, tileable over [0,1)^3 with `freq` cells per axis.
fn worley(p: vec3f, freq: i32, seed: u32) -> f32 {
  let q = p * f32(freq);
  let cell = vec3i(floor(q));
  let f = fract(q);
  var dmin = 1e9;
  for (var z = -1; z <= 1; z++) {
    for (var y = -1; y <= 1; y++) {
      for (var x = -1; x <= 1; x++) {
        let o = vec3i(x, y, z);
        let fp = vec3f(o) + rand3(wrap(cell + o, freq), seed) - f;
        dmin = min(dmin, dot(fp, fp));
      }
    }
  }
  return 1.0 - saturate(sqrt(dmin));
}

// Gradient (Perlin) noise, tileable with `period` lattice cells per axis, ~[-1, 1].
fn grad(c: vec3i, period: i32, seed: u32) -> vec3f {
  return normalize(rand3(wrap(c, period), seed) * 2.0 - 1.0 + vec3f(1e-4));
}
fn perlin(p: vec3f, period: i32, seed: u32) -> f32 {
  let q = p * f32(period);
  let i = vec3i(floor(q));
  let f = fract(q);
  let w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  let n000 = dot(grad(i + vec3i(0, 0, 0), period, seed), f - vec3f(0, 0, 0));
  let n100 = dot(grad(i + vec3i(1, 0, 0), period, seed), f - vec3f(1, 0, 0));
  let n010 = dot(grad(i + vec3i(0, 1, 0), period, seed), f - vec3f(0, 1, 0));
  let n110 = dot(grad(i + vec3i(1, 1, 0), period, seed), f - vec3f(1, 1, 0));
  let n001 = dot(grad(i + vec3i(0, 0, 1), period, seed), f - vec3f(0, 0, 1));
  let n101 = dot(grad(i + vec3i(1, 0, 1), period, seed), f - vec3f(1, 0, 1));
  let n011 = dot(grad(i + vec3i(0, 1, 1), period, seed), f - vec3f(0, 1, 1));
  let n111 = dot(grad(i + vec3i(1, 1, 1), period, seed), f - vec3f(1, 1, 1));
  let x00 = mix(n000, n100, w.x);
  let x10 = mix(n010, n110, w.x);
  let x01 = mix(n001, n101, w.x);
  let x11 = mix(n011, n111, w.x);
  return mix(mix(x00, x10, w.y), mix(x01, x11, w.y), w.z) * 1.15;
}
fn perlinFbm01(p: vec3f, period: i32, octaves: i32, seed: u32) -> f32 {
  var v = 0.0;
  var a = 0.5;
  var total = 0.0;
  var per = period;
  for (var i = 0; i < octaves; i++) {
    v += a * perlin(p, per, seed + u32(i));
    total += a;
    a *= 0.5;
    per *= 2;
  }
  return saturate(v / total * 0.5 + 0.5);
}
fn worleyFbm(p: vec3f, freq: i32, seed: u32) -> f32 {
  return worley(p, freq, seed) * 0.625 + worley(p, freq * 2, seed + 1u) * 0.25 + worley(p, freq * 4, seed + 2u) * 0.125;
}

@compute @workgroup_size(4, 4, 4)
fn genShape(@builtin(global_invocation_id) id: vec3u) {
  let p = (vec3f(id) + 0.5) / 128.0;
  let wf = worleyFbm(p, 4, 10u);
  let perlinWorley = mix(wf, 1.0, perlinFbm01(p, 4, 3, 1u));  // Perlin dilated by inverted Worley
  let g = wf;
  let b = worleyFbm(p, 8, 20u);
  let a = worleyFbm(p, 16, 30u);
  textureStore(shapeOut, id, vec4f(perlinWorley, g, b, a));
}

@compute @workgroup_size(4, 4, 4)
fn genDetail(@builtin(global_invocation_id) id: vec3u) {
  let p = (vec3f(id) + 0.5) / 32.0;
  textureStore(detailOut, id, vec4f(worleyFbm(p, 2, 40u), worleyFbm(p, 4, 50u), worleyFbm(p, 8, 60u), 1.0));
}

@compute @workgroup_size(8, 8)
fn genWeather(@builtin(global_invocation_id) id: vec3u) {
  let p = vec3f((vec2f(id.xy) + 0.5) / 1024.0, 0.0);
  let coverage = smoothstep(0.38, 0.62, perlinFbm01(p, 4, 3, 70u));  // clusters and clear gaps
  let precipitation = saturate(coverage * 1.4 - 0.5);
  let cloudType = perlinFbm01(p, 2, 2, 80u);
  textureStore(weatherOut, id.xy, vec4f(coverage, precipitation, cloudType, 1.0));
}

)";

const char* kDownsample = R"(
@group(0) @binding(0) var src: texture_3d<f32>;
@group(0) @binding(1) var dst: texture_storage_3d<rgba8unorm, write>;
@compute @workgroup_size(4, 4, 4)
fn down(@builtin(global_invocation_id) id: vec3u) {
  if (any(id >= textureDimensions(dst))) { return; }
  var s = vec4f(0.0);
  for (var z = 0u; z < 2u; z++) {
    for (var y = 0u; y < 2u; y++) {
      for (var x = 0u; x < 2u; x++) { s += textureLoad(src, vec3i(id * 2u + vec3u(x, y, z)), 0); }
    }
  }
  textureStore(dst, id, s / 8.0);
}
)";

const char* kRaymarch = R"(
@group(0) @binding(1) var shapeTex: texture_3d<f32>;
@group(0) @binding(2) var detailTex: texture_3d<f32>;
@group(0) @binding(3) var weatherTex: texture_2d<f32>;
@group(0) @binding(4) var blueTex: texture_2d<f32>;
@group(0) @binding(5) var lutTex: texture_2d<f32>;
@group(0) @binding(6) var repeatSmp: sampler;
@group(0) @binding(7) var clampSmp: sampler;
@group(0) @binding(8) var depthTex: texture_depth_2d;
@group(0) @binding(9) var outColor: texture_storage_2d<rgba16float, write>;
@group(0) @binding(10) var outDepth: texture_storage_2d<r32float, write>;
@group(0) @binding(11) var skyViewTex: texture_2d<f32>;
@group(0) @binding(12) var multiTex: texture_2d<f32>;
@group(0) @binding(13) var apTex: texture_3d<f32>;

fn remap(x: f32, a: f32, b: f32, c: f32, d: f32) -> f32 { return c + (x - a) / (b - a) * (d - c); }

// Altitude above the planet for a world point (planet centre below the world origin).
fn altitude(p: vec3f) -> f32 { return p.z + dot(p.xy, p.xy) / (2.0 * (u.camPos.w + p.z)); }
fn heightFraction(p: vec3f) -> f32 { return (altitude(p) - u.sunDir.w) / (u.sunIllum.w - u.sunDir.w); }

fn densityHeightGradient(h: f32, cloudType: f32) -> f32 {
  let stratus = saturate(remap(h, 0.0, 0.05, 0.0, 1.0) * remap(h, 0.12, 0.22, 1.0, 0.0));
  let stratocumulus = saturate(remap(h, 0.0, 0.1, 0.0, 1.0) * remap(h, 0.25, 0.55, 1.0, 0.0));
  let cumulus = saturate(remap(h, 0.0, 0.12, 0.0, 1.0) * remap(h, 0.75, 1.0, 1.0, 0.0));
  let a = mix(stratus, stratocumulus, saturate(cloudType * 2.0));
  return mix(a, cumulus, saturate((cloudType - 0.5) * 2.0));
}

// Mip levels for the shape and detail noise at the current sample: the pixel
// footprint over the texel size (26 m shape, 10.4 m detail). Set per step by the
// raymarch loop; without it distant clouds alias into streaks.
var<private> gLod = vec2f(0.0);

fn sampleDensity(p: vec3f, detail: bool) -> f32 {
  let h = heightFraction(p);
  if (h <= 0.0 || h >= 1.0) { return 0.0; }
  // Wind (accumulated on the CPU; also lifts slightly) and altitude skew.
  let wind = u.windOffset.xyz;
  let skewed = p + wind;  // rigid drift: the spec's lift and altitude shear make clouds boil
  // Weather map tile 16 km (spec 50 km): coverage changes across the view, giving
  // separate clouds instead of a uniform blanket.
  let weather = textureSampleLevel(weatherTex, repeatSmp, skewed.xy * (1.0 / 16000.0) + 0.5, 0.0).rgb;
  let coverage = saturate(weather.r * u.params0.w);
  let cloudType = saturate(mix(weather.b, u.ambient.w, 0.6));
  let shape = textureSampleLevel(shapeTex, repeatSmp, skewed * u.params0.x, gLod.x);
  let lowFbm = dot(shape.gba, vec3f(0.625, 0.25, 0.125));
  var base = remap(shape.r, -(1.0 - lowFbm), 1.0, 0.0, 1.0);
  base *= densityHeightGradient(h, cloudType);
  // Anvil: widen the top for storm clouds.
  base = pow(max(base, 0.0), mix(1.0, 0.5, saturate(remap(h, 0.65, 0.95, 0.0, 1.0)) * u.params1.x));
  var c = remap(base, 1.0 - coverage, 1.0, 0.0, 1.0) * coverage;
  if (c <= 0.001) { return 0.0; }
  if (detail) {
    // Rotated off the world axes so the 333 m tiling does not line up into rows in
    // perspective, and faded out where it is smaller than a pixel.
    let rot = mat3x3f(vec3f(0.80, 0.36, -0.48), vec3f(-0.48, 0.86, -0.16), vec3f(0.36, 0.36, 0.86));
    let dp = rot * (p + wind);  // same drift as the shape (spec 1.4x: details slide over clouds)
    let d3 = textureSampleLevel(detailTex, repeatSmp, dp * u.params0.y, gLod.y).rgb;
    let dfbm = mix(dot(d3, vec3f(0.625, 0.25, 0.125)), 0.5, saturate(gLod.y - 2.0));
    let dmod = mix(dfbm, 1.0 - dfbm, saturate(h * 5.0));  // wispy bottoms, billowy tops
    c = remap(c, dmod * 0.5, 1.0, 0.0, 1.0);  // Nubis 2017 erosion (the spec's (1 - c) factor left edges blurry)
  }
  return max(0.0, c * clamp(h * 4.0, 0.1, 1.0));
}

fn hgPhase(cosTheta: f32, g: f32) -> f32 {
  let g2 = g * g;
  return (1.0 - g2) / (12.566371 * pow(max(0.0001, 1.0 + g2 - 2.0 * g * cosTheta), 1.5));
}
fn dualLobedHG(cosTheta: f32, scale: f32) -> f32 {
  return mix(hgPhase(cosTheta, -0.3 * scale), hgPhase(cosTheta, 0.8 * scale), 0.7);
}

// Transmittance from `p` to the top of the atmosphere towards the light.
fn sunTransmittance(p: vec3f) -> vec3f {
  let rKm = (u.camPos.w + altitude(p)) * 0.001;
  let mu = clamp(dot(normalize(vec3f(p.xy, p.z + u.camPos.w)), u.sunDir.xyz), -1.0, 1.0);
  return atmoTransmittance(lutTex, clampSmp, rKm, mu);  // the atmosphere's LUT
}

// Light march towards the sun: 6 cone samples plus one long one, then 2 octaves of
// multiple scattering and the powder term.
fn marchToLight(p: vec3f, cosTheta: f32) -> f32 {
  var cone = array<vec3f, 6>(vec3f(0.38, 0.14, -0.02), vec3f(0.12, -0.42, 0.25), vec3f(-0.31, 0.29, 0.18),
                             vec3f(-0.19, -0.11, -0.45), vec3f(0.05, 0.48, -0.33), vec3f(0.44, -0.22, 0.31));
  let stepSize = 80.0;
  let l = u.sunDir.xyz;
  let up = select(vec3f(1.0, 0.0, 0.0), vec3f(0.0, 0.0, 1.0), abs(l.z) < 0.99);
  let right = normalize(cross(up, l));
  let fwd = cross(l, right);
  var optical = 0.0;
  for (var i = 0; i < 6; i++) {
    let spread = f32(i) * stepSize * 0.15;
    let k = cone[i];
    let offset = (right * k.x + fwd * k.y + l * k.z) * spread;
    optical += sampleDensity(p + l * (f32(i) + 0.5) * stepSize + offset, false) * stepSize * u.params0.z;
  }
  optical += sampleDensity(p + l * (stepSize * 12.0), false) * (stepSize * 6.0) * u.params0.z;
  var energy = 0.0;
  var a = 1.0;
  var b = 1.0;
  var c = 1.0;
  for (var n = 0; n < 4; n++) {
    energy += a * dualLobedHG(cosTheta, b) * exp(-optical * c);
    a *= 0.5;
    b *= 0.5;
    c *= 0.5;
  }
  let powder = 1.0 - exp(-optical * 2.0);
  return energy * mix(1.0, powder, saturate(-cosTheta * 0.5 + 0.5) * 0.5);
}

fn raySphere(o: vec3f, d: vec3f, r: f32) -> vec2f {
  let b = dot(o, d);
  let c = dot(o, o) - r * r;
  let disc = b * b - c;
  if (disc < 0.0) { return vec2f(-1.0); }
  let s = sqrt(disc);
  return vec2f(-b - s, -b + s);
}

@compute @workgroup_size(8, 8)
fn raymarch(@builtin(global_invocation_id) id: vec3u) {
  if (f32(id.x) >= u.size.x || f32(id.y) >= u.size.y) { return; }
  let uv = (vec2f(id.xy) + 0.5) / u.size.xy;
  // Farthest scene depth in this pixel's 2x2 full-resolution block; the composite
  // masks pixels whose own geometry is closer.
  let fp = vec2i(id.xy) * 2;
  let last = vec2i(u.size.zw) - 1;
  var depth = 0.0;
  for (var j = 0; j < 2; j++) {
    for (var i = 0; i < 2; i++) { depth = max(depth, textureLoad(depthTex, min(fp + vec2i(i, j), last), 0)); }
  }
  let rd = rayDir(uv);
  let ro = u.camPos.xyz;
  let R = u.camPos.w;
  let o = vec3f(ro.xy, ro.z + R);
  let rBottom = R + u.sunDir.w;
  let rTop = R + u.sunIllum.w;
  let rCam = R + altitude(ro);

  // Shell interval for the three camera cases.
  var tStart = 0.0;
  var tEnd = -1.0;
  let top = raySphere(o, rd, rTop);
  let bottom = raySphere(o, rd, rBottom);
  if (rCam < rBottom) {
    let ground = raySphere(o, rd, R);
    if (ground.x < 0.0 || ground.y < 0.0) { tStart = bottom.y; tEnd = top.y; }
  } else if (rCam <= rTop) {
    tStart = 0.0;
    tEnd = select(top.y, bottom.x, bottom.x > 0.0);
  } else if (top.x > 0.0) {
    tStart = top.x;
    tEnd = select(top.y, bottom.x, bottom.x > 0.0);
  }
  tEnd = min(min(tEnd, u.params1.w), sceneDistance(uv, depth));
  if (tEnd <= tStart) {
    textureStore(outColor, id.xy, vec4f(0.0, 0.0, 0.0, 1.0));
    textureStore(outDepth, id.xy, vec4f(max(tEnd, 0.0), 0.0, 0.0, 0.0));
    return;
  }

  // Spec: fine in [25, 150] m and coarse = 3 fine; finer here because coarse steps
  // longer than the eroded edges streak at grazing angles.
  let fine = clamp((tEnd - tStart) / 96.0, 20.0, 100.0);
  let coarse = fine * 2.0;
  let blue = textureLoad(blueTex, vec2i(id.xy % 64u), 0).r;
  let jitter = fract(blue + f32(u32(u.params1.z) % 32u) * 0.61803398875);
  let cosTheta = dot(rd, u.sunDir.xyz);
  let pixelAngle = length(rayDir(uv + vec2f(0.0, 1.0 / u.size.y)) - rd);

  var t = tStart + jitter * coarse;
  var inCloud = false;
  var empty = 0;
  var trans = 1.0;
  var scattered = vec3f(0.0);
  var depthSum = 0.0;
  var weightSum = 0.0;
  for (var i = 0; i < 384; i++) {
    if (t >= tEnd) { break; }
    let p = ro + rd * t;
    // At grazing angles one pixel spans much more of the (horizontal) layer in depth.
    // Gentler than 1/|rd.z|, which blurred mid elevations; the larger shapes alias less.
    let footprint = t * pixelAngle / max(sqrt(abs(rd.z)), 0.3);
    gLod = vec2f(log2(max(footprint * u.params0.x * 128.0, 1.0)), log2(max(footprint * u.params0.y * 32.0, 1.0)));
    if (!inCloud) {
      if (sampleDensity(p, false) > 0.0) {
        inCloud = true;
        empty = 0;
        t = max(tStart, t - coarse);
      } else {
        t += coarse;
      }
      continue;
    }
    let density = sampleDensity(p, true);
    if (density > 0.0) {
      empty = 0;
      let h = saturate(heightFraction(p));
      let sigmaE = density * u.params0.z;
      let stepTrans = exp(-sigmaE * fine);
      let direct = u.sunIllum.rgb * sunTransmittance(p) * marchToLight(p, cosTheta);
      // Sky ambient: the atmosphere's (spec section 4) by day, the fog-based moonlight
      // at night. Spec 0.15 at the base: too dark without deep multiple scattering.
      let rKm = kBottom + altitude(p) * 0.001;
      let skyAmbient = sampleSkyAmbientIrradiance(skyViewTex, multiTex, clampSmp, rKm, u.atmo.xyz, 14.0);
      let ambient = mix(u.ambient.rgb, skyAmbient, u.atmo.w) * mix(0.35, 1.0, h);
      let w = trans * (1.0 - stepTrans);
      scattered += trans * (direct + ambient) * (1.0 - stepTrans);
      depthSum += t * w;
      weightSum += w;
      trans *= stepTrans;
      if (trans < 0.01) {
        trans = 0.0;
        break;
      }
    } else {
      empty++;
      if (empty >= 6) { inCloud = false; }
    }
    t += fine;
  }

  // Aerial perspective from the game's fog: haze between the camera and the cloud.
  let tCloud = select(tEnd, depthSum / weightSum, weightSum > 1e-4);
  let hazeTrans = exp(-tCloud / u.haze.w);
  let fogged = scattered * hazeTrans + u.haze.rgb * (1.0 - hazeTrans) * (1.0 - trans);
  // Spec 4.1: the atmosphere's aerial perspective between camera and cloud.
  let ap = sampleAerialPerspective(apTex, clampSmp, uv, tCloud, 64000.0);
  let rgb = mix(fogged, scattered * ap.a + ap.rgb * (1.0 - trans), u.atmo.w);
  textureStore(outColor, id.xy, vec4f(rgb, trans));
  textureStore(outDepth, id.xy, vec4f(tCloud, 0.0, 0.0, 0.0));
}
)";

const char* kReproject = R"(
@group(0) @binding(1) var curColor: texture_2d<f32>;
@group(0) @binding(2) var curDepth: texture_2d<f32>;
@group(0) @binding(3) var history: texture_2d<f32>;
@group(0) @binding(4) var clampSmp: sampler;
@group(0) @binding(5) var outHistory: texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn reproject(@builtin(global_invocation_id) id: vec3u) {
  if (f32(id.x) >= u.size.z || f32(id.y) >= u.size.w) { return; }
  let uv = (vec2f(id.xy) + 0.5) / u.size.zw;
  let current = textureSampleLevel(curColor, clampSmp, uv, 0.0);
  let hc = vec2i(uv * u.size.xy);
  let last = vec2i(u.size.xy) - 1;
  var m1 = vec4f(0.0);
  var m2 = vec4f(0.0);
  for (var j = -1; j <= 1; j++) {
    for (var i = -1; i <= 1; i++) {
      let s = textureLoad(curColor, clamp(hc + vec2i(i, j), vec2i(0), last), 0);
      m1 += s;
      m2 += s * s;
    }
  }
  let mu = m1 / 9.0;
  let sigma = sqrt(max(m2 / 9.0 - mu * mu, vec4f(0.0)));
  var result = current;
  if (u.misc.y > 0.5) {
    let t = textureLoad(curDepth, clamp(hc, vec2i(0), last), 0).r;
    let p = u.camPos.xyz + rayDir(uv) * t;
    // Density is sampled at p + windOffset, so features move against the offset: the
    // point seen at p now was at p + (offset - prevOffset) last frame.
    let prevP = p + (u.windOffset.xyz - u.prevWindOffset.xyz);
    let clip = u.prevViewProj * vec4f(prevP, 1.0);
    if (clip.w > 0.0) {
      let ndc = clip.xy / clip.w;
      let prevUv = vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
      if (all(prevUv >= vec2f(0.0)) && all(prevUv <= vec2f(1.0))) {
        let hist = textureSampleLevel(history, clampSmp, prevUv, 0.0);
        let clamped = clamp(hist, mu - 1.25 * sigma, mu + 1.25 * sigma);
        result = mix(clamped, current, 0.1);
      }
    }
  }
  textureStore(outHistory, id.xy, result);
}
)";

const char* kComposite = R"(
@group(0) @binding(1) var accumulated: texture_2d<f32>;
@group(0) @binding(2) var cloudDepth: texture_2d<f32>;
@group(0) @binding(3) var depthTex: texture_depth_2d;
struct V { @builtin(position) pos: vec4f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> V {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;
  return V(vec4f(p, 0.0, 1.0));
}
// Blend: out = scene * a + rgb (a = cloud transmittance).
@fragment fn fs(v: V) -> @location(0) vec4f {
  let p = vec2i(v.pos.xy);
  let uv = v.pos.xy / u.size.zw;
  let c = textureLoad(accumulated, p, 0);
  let depth = textureLoad(depthTex, p, 0);
  let tc = textureLoad(cloudDepth, min(p / 2, vec2i(u.size.xy) - 1), 0).r;
  if (sceneDistance(uv, depth) < tc) { return vec4f(0.0, 0.0, 0.0, 1.0); }  // own geometry in front
  // Tone-map the coverage-normalized radiance, then weight by coverage: the curve is
  // concave, so mapping the premultiplied value would brighten thin edges (halos).
  let coverage = 1.0 - c.a;
  let radiance = c.rgb / max(coverage, 1e-3);
  return vec4f((1.0 - exp(-radiance * u.misc.x)) * coverage, c.a);
}
)";

// ---------------------------------------------------------------------------

struct State {
  wgpu::ComputePipeline raymarch, reproject;
  wgpu::RenderPipeline composite;
  wgpu::BindGroupLayout raymarchLayout, reprojectLayout, compositeLayout;
  wgpu::Buffer uniforms;
  wgpu::Sampler repeatSampler, clampSampler;
  wgpu::Texture shape, detail, weather, blue, lut;
  wgpu::TextureView shapeView, detailView, weatherView, blueView;
  // Per resolution.
  uint32_t width = 0, height = 0;
  wgpu::Texture color, depth, history[2];
  wgpu::TextureView colorView, depthView, historyView[2];
  int current = 0;
  bool historyValid = false;
  // Per frame.
  float prevViewProj[16] = {};
  float prevCamera[3] = {};
  double windOffset[3] = {}, prevWindOffset[3] = {};
  double lastTime = -1;
  uint32_t frame = 0;
} *g_state = nullptr;

wgpu::ShaderModule Module(const wgpu::Device& device, const std::string& code) {
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor desc{};
  desc.nextInChain = &wgsl;
  return device.CreateShaderModule(&desc);
}

wgpu::BindGroupLayoutEntry UniformEntry(uint32_t binding, wgpu::ShaderStage stages) {
  wgpu::BindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = stages;
  e.buffer.type = wgpu::BufferBindingType::Uniform;
  e.buffer.minBindingSize = sizeof(Uniforms);
  return e;
}
wgpu::BindGroupLayoutEntry TextureEntry(uint32_t binding, wgpu::ShaderStage stages, wgpu::TextureSampleType type,
                                        wgpu::TextureViewDimension dim = wgpu::TextureViewDimension::e2D) {
  wgpu::BindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = stages;
  e.texture.sampleType = type;
  e.texture.viewDimension = dim;
  return e;
}
wgpu::BindGroupLayoutEntry StorageEntry(uint32_t binding, wgpu::TextureFormat format,
                                        wgpu::TextureViewDimension dim = wgpu::TextureViewDimension::e2D) {
  wgpu::BindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = wgpu::ShaderStage::Compute;
  e.storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  e.storageTexture.format = format;
  e.storageTexture.viewDimension = dim;
  return e;
}
wgpu::BindGroupLayoutEntry SamplerEntry(uint32_t binding, wgpu::ShaderStage stages) {
  wgpu::BindGroupLayoutEntry e{};
  e.binding = binding;
  e.visibility = stages;
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
wgpu::Texture MakeTexture(const wgpu::Device& device, uint32_t w, uint32_t h, uint32_t d, wgpu::TextureFormat format,
                          wgpu::TextureUsage usage, wgpu::TextureDimension dim = wgpu::TextureDimension::e2D,
                          uint32_t mips = 1) {
  wgpu::TextureDescriptor td{};
  td.mipLevelCount = mips;
  td.size = {w, h, d};
  td.format = format;
  td.usage = usage;
  td.dimension = dim;
  return device.CreateTexture(&td);
}

// 64x64 blue noise by void and cluster (Ulichney): place points in the largest voids,
// rank the initial set by removing tightest clusters, then rank the rest by filling
// the largest voids. Energy is a toroidal Gaussian (sigma 1.5).
std::vector<uint8_t> BlueNoise() {
  constexpr int N = kBlueSize, kCount = N * N;
  float kernel[N][N];
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      int dx = std::min(x, N - x), dy = std::min(y, N - y);
      kernel[y][x] = std::exp(-(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
    }
  std::vector<float> energy(kCount, 0.0f);
  std::vector<uint8_t> on(kCount, 0);
  auto toggle = [&](int i, float sign) {
    int px = i % N, py = i / N;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) energy[y * N + x] += sign * kernel[(y - py + N) % N][(x - px + N) % N];
  };
  auto extreme = [&](bool wantOn, bool maximum) {
    int best = -1;
    for (int i = 0; i < kCount; ++i)
      if ((on[i] != 0) == wantOn && (best < 0 || (maximum ? energy[i] > energy[best] : energy[i] < energy[best]))) best = i;
    return best;
  };
  std::vector<int> rank(kCount, 0);
  std::mt19937 rng(1234);
  int initial = kCount / 10;
  int first = static_cast<int>(rng() % kCount);
  on[first] = 1;
  toggle(first, 1.0f);
  for (int n = 1; n < initial; ++n) {
    int v = extreme(false, false);
    on[v] = 1;
    toggle(v, 1.0f);
  }
  std::vector<uint8_t> initialOn = on;
  std::vector<float> initialEnergy = energy;
  for (int r = initial - 1; r >= 0; --r) {  // phase 1
    int c = extreme(true, true);
    on[c] = 0;
    toggle(c, -1.0f);
    rank[c] = r;
  }
  on = initialOn;
  energy = initialEnergy;
  for (int r = initial; r < kCount; ++r) {  // phases 2 and 3
    int v = extreme(false, false);
    on[v] = 1;
    toggle(v, 1.0f);
    rank[v] = r;
  }
  std::vector<uint8_t> out(kCount);
  for (int i = 0; i < kCount; ++i) out[i] = static_cast<uint8_t>(rank[i] * 256 / kCount);
  return out;
}

float Smooth(float a, float b, float x) {
  float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// Per CWeather type (EXTRASUNNY_LA ... EXTRACOLOURS_2): coverage multiplier and
// cloud type bias (0 stratus, 0.5 stratocumulus, 1 cumulus).
struct WeatherLook {
  float coverage, type;
};
WeatherLook Look(int weather) {
  static const WeatherLook kLooks[] = {
      {0.55f, 0.9f}, {0.72f, 0.8f}, {0.60f, 0.7f}, {0.72f, 0.6f}, {1.2f, 0.5f},  // LA
      {0.72f, 0.8f}, {0.55f, 0.9f}, {1.20f, 0.5f}, {1.6f, 0.3f}, {1.4f, 0.1f},   // SF
      {0.65f, 0.9f}, {0.45f, 1.0f}, {1.10f, 0.6f},                               // Vegas
      {0.55f, 0.9f}, {0.72f, 0.8f}, {1.20f, 0.5f}, {1.6f, 0.4f},                 // countryside
      {0.40f, 1.0f}, {0.60f, 0.9f}, {1.00f, 0.3f},                               // desert
      {0.8f, 0.6f},  {0.8f, 0.6f}, {0.8f, 0.6f},                              // underwater, extra colours
  };
  return weather >= 0 && weather < static_cast<int>(std::size(kLooks)) ? kLooks[weather] : WeatherLook{0.8f, 0.6f};
}

void EnsureTargets(const wgpu::Device& device, uint32_t width, uint32_t height) {
  State* s = g_state;
  if (s->width == width && s->height == height && s->color) return;
  s->width = width;
  s->height = height;
  uint32_t hw = std::max(width / 2, 1u), hh = std::max(height / 2, 1u);
  auto storage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding;
  s->color = MakeTexture(device, hw, hh, 1, wgpu::TextureFormat::RGBA16Float, storage);
  s->depth = MakeTexture(device, hw, hh, 1, wgpu::TextureFormat::R32Float, storage);
  s->colorView = s->color.CreateView();
  s->depthView = s->depth.CreateView();
  for (int i = 0; i < 2; ++i) {
    s->history[i] = MakeTexture(device, width, height, 1, wgpu::TextureFormat::RGBA16Float, storage);
    s->historyView[i] = s->history[i].CreateView();
  }
  s->historyValid = false;
}

}  // namespace

bool Init(const wgpu::Device& device, const wgpu::Queue& queue, wgpu::TextureFormat target) {
  if (g_state) return true;
  auto* s = new State();
  g_state = s;
  const auto compute = wgpu::ShaderStage::Compute;
  const auto fragment = wgpu::ShaderStage::Fragment;
  const auto f32 = wgpu::TextureSampleType::Float;
  const auto unfilterable = wgpu::TextureSampleType::UnfilterableFloat;
  const auto depthType = wgpu::TextureSampleType::Depth;
  const auto d3 = wgpu::TextureViewDimension::e3D;

  // Static textures and their generation.
  auto genUsage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc;
  constexpr uint32_t kShapeMips = 8, kDetailMips = 6;  // down to 1^3
  s->shape = MakeTexture(device, kShapeSize, kShapeSize, kShapeSize, wgpu::TextureFormat::RGBA8Unorm, genUsage,
                         wgpu::TextureDimension::e3D, kShapeMips);
  s->detail = MakeTexture(device, kDetailSize, kDetailSize, kDetailSize, wgpu::TextureFormat::RGBA8Unorm, genUsage,
                          wgpu::TextureDimension::e3D, kDetailMips);
  s->weather = MakeTexture(device, kWeatherSize, kWeatherSize, 1, wgpu::TextureFormat::RGBA8Unorm, genUsage);
  s->blue = MakeTexture(device, kBlueSize, kBlueSize, 1, wgpu::TextureFormat::R8Unorm,
                        wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst);
  s->shapeView = s->shape.CreateView();
  s->detailView = s->detail.CreateView();
  s->weatherView = s->weather.CreateView();
  s->blueView = s->blue.CreateView();

  wgpu::BindGroupLayout genLayout =
      Layout(device, {StorageEntry(0, wgpu::TextureFormat::RGBA8Unorm, d3), StorageEntry(1, wgpu::TextureFormat::RGBA8Unorm, d3),
                      StorageEntry(2, wgpu::TextureFormat::RGBA8Unorm)});
  wgpu::ShaderModule genModule = Module(device, kGenerate);
  auto mipView = [](const wgpu::Texture& t, uint32_t level) {
    wgpu::TextureViewDescriptor vd{};
    vd.baseMipLevel = level;
    vd.mipLevelCount = 1;
    return t.CreateView(&vd);
  };
  wgpu::TextureView shapeLevel0 = mipView(s->shape, 0), detailLevel0 = mipView(s->detail, 0);
  wgpu::BindGroupEntry ge[3] = {};
  const wgpu::TextureView* genViews[3] = {&shapeLevel0, &detailLevel0, &s->weatherView};
  for (int i = 0; i < 3; ++i) ge[i].binding = i, ge[i].textureView = *genViews[i];
  wgpu::BindGroupDescriptor gbd{};
  gbd.layout = genLayout;
  gbd.entryCount = 3;
  gbd.entries = ge;
  wgpu::BindGroup genGroup = device.CreateBindGroup(&gbd);
  struct Job {
    const char* entry;
    uint32_t x, y, z;
  } jobs[] = {{"genShape", kShapeSize / 4, kShapeSize / 4, kShapeSize / 4},
              {"genDetail", kDetailSize / 4, kDetailSize / 4, kDetailSize / 4},
              {"genWeather", kWeatherSize / 8, kWeatherSize / 8, 1}};
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
  for (const Job& j : jobs) {
    wgpu::ComputePipeline p = Compute(device, genModule, j.entry, genLayout);
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(p);
    pass.SetBindGroup(0, genGroup);
    pass.DispatchWorkgroups(j.x, j.y, j.z);
    pass.End();
  }
  // Mip chains of the 3D noise (2x2x2 box filter).
  {
    wgpu::BindGroupLayoutEntry entries[2] = {};
    entries[0] = TextureEntry(0, compute, unfilterable, d3);
    entries[1] = StorageEntry(1, wgpu::TextureFormat::RGBA8Unorm, d3);
    wgpu::BindGroupLayout downLayout = Layout(device, {entries[0], entries[1]});
    wgpu::ComputePipeline down = Compute(device, Module(device, kDownsample), "down", downLayout);
    struct Chain {
      const wgpu::Texture* texture;
      uint32_t size, mips;
    } chains[] = {{&s->shape, kShapeSize, kShapeMips}, {&s->detail, kDetailSize, kDetailMips}};
    for (const Chain& c : chains) {
      for (uint32_t level = 1; level < c.mips; ++level) {
        wgpu::BindGroupEntry e[2] = {};
        e[0].binding = 0;
        e[0].textureView = mipView(*c.texture, level - 1);
        e[1].binding = 1;
        e[1].textureView = mipView(*c.texture, level);
        wgpu::BindGroupDescriptor bd{};
        bd.layout = downLayout;
        bd.entryCount = 2;
        bd.entries = e;
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
        pass.SetPipeline(down);
        pass.SetBindGroup(0, device.CreateBindGroup(&bd));
        uint32_t n = std::max(c.size >> level, 1u);
        pass.DispatchWorkgroups((n + 3) / 4, (n + 3) / 4, (n + 3) / 4);
        pass.End();
      }
    }
  }
  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);
  std::vector<uint8_t> blue = BlueNoise();
  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = s->blue;
  wgpu::TexelCopyBufferLayout bl{};
  bl.bytesPerRow = kBlueSize;
  wgpu::Extent3D blueSize{kBlueSize, kBlueSize, 1};
  queue.WriteTexture(&dst, blue.data(), blue.size(), &bl, &blueSize);

  // Per-frame pipelines.
  wgpu::SamplerDescriptor sd{};
  sd.magFilter = sd.minFilter = wgpu::FilterMode::Linear;
  sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  sd.addressModeU = sd.addressModeV = sd.addressModeW = wgpu::AddressMode::Repeat;
  s->repeatSampler = device.CreateSampler(&sd);
  sd.addressModeU = sd.addressModeV = sd.addressModeW = wgpu::AddressMode::ClampToEdge;
  s->clampSampler = device.CreateSampler(&sd);
  wgpu::BufferDescriptor ub{};
  ub.size = sizeof(Uniforms);
  ub.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  s->uniforms = device.CreateBuffer(&ub);

  s->raymarchLayout = Layout(device, {UniformEntry(0, compute), TextureEntry(1, compute, f32, d3),
                                      TextureEntry(2, compute, f32, d3), TextureEntry(3, compute, f32),
                                      TextureEntry(4, compute, f32), TextureEntry(5, compute, f32),
                                      SamplerEntry(6, compute), SamplerEntry(7, compute),
                                      TextureEntry(8, compute, depthType),
                                      StorageEntry(9, wgpu::TextureFormat::RGBA16Float),
                                      StorageEntry(10, wgpu::TextureFormat::R32Float), TextureEntry(11, compute, f32),
                                      TextureEntry(12, compute, f32), TextureEntry(13, compute, f32, d3)});
  s->raymarch = Compute(device, Module(device, std::string(atmosphere::CommonWgsl()) + kCommon + kRaymarch), "raymarch", s->raymarchLayout);
  s->reprojectLayout = Layout(device, {UniformEntry(0, compute), TextureEntry(1, compute, f32),
                                       TextureEntry(2, compute, unfilterable), TextureEntry(3, compute, f32),
                                       SamplerEntry(4, compute), StorageEntry(5, wgpu::TextureFormat::RGBA16Float)});
  s->reproject =
      Compute(device, Module(device, std::string(kCommon) + kReproject), "reproject", s->reprojectLayout);

  s->compositeLayout = Layout(device, {UniformEntry(0, fragment), TextureEntry(1, fragment, unfilterable),
                                       TextureEntry(2, fragment, unfilterable), TextureEntry(3, fragment, depthType)});
  wgpu::ShaderModule compositeModule = Module(device, std::string(kCommon) + kComposite);
  wgpu::BlendState blend{};
  blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::SrcAlpha};
  blend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::Zero, wgpu::BlendFactor::One};
  wgpu::ColorTargetState color{};
  color.format = target;
  color.blend = &blend;
  wgpu::FragmentState fs{};
  fs.module = compositeModule;
  fs.entryPoint = "fs";
  fs.targetCount = 1;
  fs.targets = &color;
  wgpu::RenderPipelineDescriptor rp{};
  rp.layout = PipelineLayoutOf(device, s->compositeLayout);
  rp.vertex.module = compositeModule;
  rp.vertex.entryPoint = "vs";
  rp.fragment = &fs;
  s->composite = device.CreateRenderPipeline(&rp);
  return true;
}

void Shutdown() {
  delete g_state;
  g_state = nullptr;
}

wgpu::Texture ShapeNoise() { return g_state ? g_state->shape : nullptr; }
wgpu::Texture DetailNoise() { return g_state ? g_state->detail : nullptr; }

void ResetHistory() {
  if (g_state) g_state->historyValid = false;
}

void Render(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::CommandEncoder& encoder,
            const Frame& f, uint32_t width, uint32_t height, const wgpu::TextureView& sceneDepth) {
  State* s = g_state;
  if (!s) return;
  EnsureTargets(device, width, height);
  uint32_t hw = std::max(width / 2, 1u), hh = std::max(height / 2, 1u);

  // A camera cut invalidates the history.
  float moved = std::hypot(f.camera[0] - s->prevCamera[0], f.camera[1] - s->prevCamera[1],
                           f.camera[2] - s->prevCamera[2]);
  if (moved > 50.0f) s->historyValid = false;

  // Light: the sun by day, a dim cool moon opposite it at night.
  const float* sun = f.game.sun;
  float elevation = sun[2];
  bool night = elevation < -0.1f;
  float lightDir[3];
  for (int i = 0; i < 3; ++i) lightDir[i] = night ? -sun[i] : sun[i];
  if (lightDir[0] == 0 && lightDir[1] == 0 && lightDir[2] == 0) lightDir[2] = 1;
  float rain = std::clamp(f.game.weather[3], 0.0f, 1.0f);
  float dayStrength = 14.0f * Smooth(-0.1f, 0.05f, elevation);
  float illum[3];
  const float moon[3] = {0.55f, 0.65f, 0.9f};
  for (int i = 0; i < 3; ++i) illum[i] = (night ? 0.9f * moon[i] : dayStrength) * (1.0f - 0.65f * rain);

  // Weather: blend the old and new looks.
  int oldW = static_cast<int>(f.game.weather[0]), newW = static_cast<int>(f.game.weather[1]);
  float blend = std::clamp(f.game.weather[2], 0.0f, 1.0f);
  WeatherLook a = Look(oldW), b = Look(newW);
  float coverage = a.coverage * (1 - blend) + b.coverage * blend + 0.4f * rain;
  float type = a.type * (1 - blend) + b.type * blend;

  // Wind: integrate the current speed over the game time since the last frame.
  double now = f.game.time[1];
  double dt = s->lastTime < 0 ? 0 : std::clamp(now - s->lastTime, 0.0, 0.25);
  s->lastTime = now;
  double speed = 3.0 + 9.0 * std::clamp(f.game.env[1], 0.0f, 1.0f);  // m/s
  const double dir[3] = {0.96, 0.28, 0.0};
  std::memcpy(s->prevWindOffset, s->windOffset, sizeof(s->windOffset));
  s->windOffset[0] += dir[0] * speed * dt;
  s->windOffset[1] += dir[1] * speed * dt;


  Uniforms u{};
  std::memcpy(u.invViewProj, f.invViewProj, sizeof(u.invViewProj));
  std::memcpy(u.prevViewProj, s->historyValid ? s->prevViewProj : f.viewProj, sizeof(u.prevViewProj));
  std::memcpy(u.camPos, f.camera, sizeof(f.camera));
  u.camPos[3] = kPlanetRadius;
  std::memcpy(u.sunDir, lightDir, sizeof(lightDir));
  u.sunDir[3] = kCloudBottom;
  std::memcpy(u.sunIllum, illum, sizeof(illum));
  u.sunIllum[3] = kCloudTop;
  for (int i = 0; i < 3; ++i) {
    u.windDir[i] = static_cast<float>(dir[i]);
    u.windOffset[i] = static_cast<float>(s->windOffset[i]);
    u.prevWindOffset[i] = static_cast<float>(s->prevWindOffset[i]);
  }
  u.windDir[3] = static_cast<float>(speed);
  u.params0[0] = kShapeScale;
  u.params0[1] = kDetailScale;
  u.params0[2] = kExtinction * (1.0f + rain);
  u.params0[3] = coverage;
  u.params1[0] = std::clamp((rain - 0.3f) * 2.0f, 0.0f, 1.0f);  // anvils in storms
  u.params1[1] = static_cast<float>(std::fmod(now, 3600.0));
  u.params1[2] = static_cast<float>(s->frame++);
  u.params1[3] = kMaxDistance;
  for (int i = 0; i < 3; ++i) {
    // Sky ambient: the whole sky dome is much paler than the game's fog colour (its
    // horizon colour, a saturated blue at midday), so keep only 35% of its saturation.
    float lum = 0.2126f * f.fogColor[0] + 0.7152f * f.fogColor[1] + 0.0722f * f.fogColor[2];
    u.ambient[i] = (lum + (f.fogColor[i] - lum) * 0.35f) * (night ? 0.6f : 1.3f) * (1.0f - 0.4f * rain);
    u.haze[i] = f.fogColor[i];
  }
  u.ambient[3] = type;
  u.haze[3] = std::max(f.fogEnd, 100.0f) * 25.0f;  // aerial perspective distance (m)
  u.size[0] = static_cast<float>(hw);
  u.size[1] = static_cast<float>(hh);
  u.size[2] = static_cast<float>(width);
  u.size[3] = static_cast<float>(height);
  u.misc[0] = 1.7f;  // exposure
  u.misc[1] = s->historyValid ? 1.0f : 0.0f;
  u.misc[2] = rain;
  std::memcpy(u.atmo, f.game.sun, sizeof(float) * 3);
  u.atmo[3] = f.atmosphere ? atmosphere::DayFactor(f.game.sun) : 0.0f;
  queue.WriteBuffer(s->uniforms, 0, &u, sizeof(u));

  // Raymarch (half resolution).
  {
    const atmosphere::Views& atmo = atmosphere::GetViews();
    wgpu::BindGroupEntry e[14] = {};
    for (int i = 0; i < 14; ++i) e[i].binding = i;
    e[0].buffer = s->uniforms;
    e[0].size = sizeof(Uniforms);
    e[1].textureView = s->shapeView;
    e[2].textureView = s->detailView;
    e[3].textureView = s->weatherView;
    e[4].textureView = s->blueView;
    e[5].textureView = atmo.transmittance;
    e[6].sampler = s->repeatSampler;
    e[7].sampler = s->clampSampler;
    e[8].textureView = sceneDepth;
    e[9].textureView = s->colorView;
    e[10].textureView = s->depthView;
    wgpu::BindGroupDescriptor bd{};
    bd.layout = s->raymarchLayout;
    e[11].textureView = atmo.skyView;
    e[12].textureView = atmo.multiScattering;
    e[13].textureView = atmo.aerialPerspective;
    bd.entryCount = 14;
    bd.entries = e;
    wgpu::BindGroup group = device.CreateBindGroup(&bd);
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(s->raymarch);
    pass.SetBindGroup(0, group);
    pass.DispatchWorkgroups((hw + 7) / 8, (hh + 7) / 8, 1);
    pass.End();
  }
  // Temporal reprojection (full resolution).
  {
    int prev = s->current, next = 1 - s->current;
    wgpu::BindGroupEntry e[6] = {};
    for (int i = 0; i < 6; ++i) e[i].binding = i;
    e[0].buffer = s->uniforms;
    e[0].size = sizeof(Uniforms);
    e[1].textureView = s->colorView;
    e[2].textureView = s->depthView;
    e[3].textureView = s->historyView[prev];
    e[4].sampler = s->clampSampler;
    e[5].textureView = s->historyView[next];
    wgpu::BindGroupDescriptor bd{};
    bd.layout = s->reprojectLayout;
    bd.entryCount = 6;
    bd.entries = e;
    wgpu::BindGroup group = device.CreateBindGroup(&bd);
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(s->reproject);
    pass.SetBindGroup(0, group);
    pass.DispatchWorkgroups((width + 7) / 8, (height + 7) / 8, 1);
    pass.End();
    s->current = next;
  }
  std::memcpy(s->prevViewProj, f.viewProj, sizeof(s->prevViewProj));
  std::memcpy(s->prevCamera, f.camera, sizeof(s->prevCamera));
  s->historyValid = true;

  if (s->frame % 600 == 0)
    Logf("[clouds] coverage %.2f type %.2f light (%.2f %.2f %.2f)%s ambient (%.2f %.2f %.2f) haze %.0f m, wind %.1f m/s\n",
         coverage, type, illum[0], illum[1], illum[2], night ? " moon" : "", u.ambient[0], u.ambient[1], u.ambient[2],
         u.haze[3], speed);
}

void Composite(const wgpu::Device& device, const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
               const wgpu::TextureView& sceneDepth) {
  State* s = g_state;
  if (!s || !s->color) return;
  wgpu::BindGroupEntry e[4] = {};
  for (int i = 0; i < 4; ++i) e[i].binding = i;
  e[0].buffer = s->uniforms;
  e[0].size = sizeof(Uniforms);
  e[1].textureView = s->historyView[s->current];
  e[2].textureView = s->depthView;
  e[3].textureView = sceneDepth;
  wgpu::BindGroupDescriptor bd{};
  bd.layout = s->compositeLayout;
  bd.entryCount = 4;
  bd.entries = e;
  wgpu::BindGroup group = device.CreateBindGroup(&bd);
  wgpu::RenderPassColorAttachment color{};
  color.view = target;
  color.loadOp = wgpu::LoadOp::Load;
  color.storeOp = wgpu::StoreOp::Store;
  wgpu::RenderPassDescriptor pd{};
  pd.colorAttachmentCount = 1;
  pd.colorAttachments = &color;
  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pd);
  pass.SetPipeline(s->composite);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}

}  // namespace agpu::wgb::clouds
