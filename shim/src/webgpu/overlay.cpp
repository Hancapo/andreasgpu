#include "webgpu/overlay.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace agpu::wgb::overlay {
namespace {

// Glyph rows top to bottom; bit (row * 3 + col) is set for a lit pixel.
constexpr const char* kGlyphs[16][5] = {
    {"000", "000", "000", "000", "000"},  // space
    {"111", "101", "101", "101", "111"},  // 0
    {"010", "110", "010", "010", "111"},  // 1
    {"111", "001", "111", "100", "111"},  // 2
    {"111", "001", "111", "001", "111"},  // 3
    {"101", "101", "111", "001", "001"},  // 4
    {"111", "100", "111", "001", "111"},  // 5
    {"111", "100", "111", "101", "111"},  // 6
    {"111", "001", "001", "001", "001"},  // 7
    {"111", "101", "111", "101", "111"},  // 8
    {"111", "101", "111", "001", "111"},  // 9
    {"000", "000", "000", "000", "010"},  // .
    {"111", "100", "111", "100", "100"},  // F
    {"111", "101", "111", "100", "100"},  // P
    {"111", "100", "111", "001", "111"},  // S
    {"101", "111", "111", "101", "101"},  // M
};

uint32_t GlyphIndex(char c) {
  if (c >= '0' && c <= '9') return 1 + (c - '0');
  switch (c) {
    case '.': return 11;
    case 'F': return 12;
    case 'P': return 13;
    case 'S': return 14;
    case 'M': return 15;
    default: return 0;
  }
}

constexpr uint32_t kMaxChars = 32;

// Must match struct O in the shader.
struct Uniforms {
  uint32_t chars[4];  // 4 bits per character, 8 per word
  uint32_t count, scale, originX, originY;
  float targetWidth, targetHeight, pad[2];
};

struct State {
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer uniforms;
  wgpu::BindGroup bindGroup;
  Uniforms data{};
} g;

std::string Shader() {
  std::string glyphs;
  for (int i = 0; i < 16; ++i) {
    uint32_t mask = 0;
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3; ++col)
        if (kGlyphs[i][row][col] == '1') mask |= 1u << (row * 3 + col);
    glyphs += (i ? ", " : "") + std::to_string(mask) + "u";
  }
  return R"(
struct O { chars: vec4u, info: vec4u, size: vec4f };
@group(0) @binding(0) var<uniform> o: O;
const GLYPHS = array<u32, 16>()" + glyphs + R"();

// Box: one cell of padding, then 4x6 cells per character (3x5 glyph + spacing).
fn boxSize() -> vec2f { return vec2f(f32(o.info.x * 4u + 1u), 7.0) * f32(o.info.y); }

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let corners = array<vec2f, 6>(vec2f(0, 0), vec2f(1, 0), vec2f(0, 1), vec2f(0, 1), vec2f(1, 0), vec2f(1, 1));
  let px = vec2f(f32(o.info.z), f32(o.info.w)) + corners[i] * boxSize();
  let ndc = px / o.size.xy * 2.0 - 1.0;
  return vec4f(ndc.x, -ndc.y, 0.0, 1.0);
}

@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let background = vec4f(0.0, 0.0, 0.0, 0.55);
  let cell = vec2i(floor((pos.xy - vec2f(f32(o.info.z), f32(o.info.w))) / f32(o.info.y))) - vec2i(1, 1);
  if (cell.x < 0 || cell.y < 0 || cell.y >= 5) { return background; }
  let index = u32(cell.x) / 4u;
  let col = u32(cell.x) % 4u;
  if (col == 3u || index >= o.info.x) { return background; }
  let code = (o.chars[index / 8u] >> ((index % 8u) * 4u)) & 15u;
  if (((GLYPHS[code] >> (u32(cell.y) * 3u + col)) & 1u) == 0u) { return background; }
  return vec4f(1.0, 1.0, 0.2, 1.0);
}
)";
}

}  // namespace

bool Init(const wgpu::Device& device, wgpu::TextureFormat format) {
  std::string code = Shader();
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = {code.data(), code.size()};
  wgpu::ShaderModuleDescriptor moduleDesc{};
  moduleDesc.nextInChain = &wgsl;
  wgpu::ShaderModule module = device.CreateShaderModule(&moduleDesc);

  wgpu::BlendState blend{};
  blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha};
  blend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha};
  wgpu::ColorTargetState target{};
  target.format = format;
  target.blend = &blend;
  wgpu::FragmentState fragment{};
  fragment.module = module;
  fragment.entryPoint = "fs";
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor desc{};
  desc.vertex.module = module;
  desc.vertex.entryPoint = "vs";
  desc.fragment = &fragment;
  g.pipeline = device.CreateRenderPipeline(&desc);  // auto layout

  wgpu::BufferDescriptor bufferDesc{};
  bufferDesc.size = sizeof(Uniforms);
  bufferDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  g.uniforms = device.CreateBuffer(&bufferDesc);
  wgpu::BindGroupEntry entry{};
  entry.binding = 0;
  entry.buffer = g.uniforms;
  entry.size = sizeof(Uniforms);
  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.layout = g.pipeline.GetBindGroupLayout(0);
  bgDesc.entryCount = 1;
  bgDesc.entries = &entry;
  g.bindGroup = device.CreateBindGroup(&bgDesc);
  return g.pipeline && g.bindGroup;
}

void SetText(const char* text) {
  std::memset(g.data.chars, 0, sizeof(g.data.chars));
  uint32_t n = 0;
  for (; text[n] && n < kMaxChars; ++n) g.data.chars[n / 8] |= GlyphIndex(text[n]) << ((n % 8) * 4);
  g.data.count = n;
}

void Draw(const wgpu::Queue& queue, const wgpu::RenderPassEncoder& pass, uint32_t width, uint32_t height) {
  if (!g.pipeline || !g.data.count) return;
  g.data.scale = std::max(2u, height / 270);
  g.data.originX = g.data.originY = 2 * g.data.scale;
  g.data.targetWidth = static_cast<float>(width);
  g.data.targetHeight = static_cast<float>(height);
  queue.WriteBuffer(g.uniforms, 0, &g.data, sizeof(g.data));
  pass.SetPipeline(g.pipeline);
  pass.SetBindGroup(0, g.bindGroup);
  pass.Draw(6);
}

}  // namespace agpu::wgb::overlay
