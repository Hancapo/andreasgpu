#pragma once
// One line of debug text drawn over the presented frame (FPS counter). The font is
// a 3x5 pixel font baked into the shader: digits, space, '.', 'F', 'P', 'S', 'M'.
#include <webgpu/webgpu_cpp.h>

namespace agpu::wgb::overlay {

bool Init(const wgpu::Device& device, wgpu::TextureFormat format);
// Up to 32 characters; others than the supported ones show as spaces.
void SetText(const char* text);
// Draws into `pass`, a render pass on a target of `width` x `height` pixels.
void Draw(const wgpu::Queue& queue, const wgpu::RenderPassEncoder& pass, uint32_t width, uint32_t height);

}  // namespace agpu::wgb::overlay
