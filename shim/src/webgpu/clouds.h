#pragma once
// Volumetric clouds after Schneider (Nubis, 2015/2017) for the density model and
// Hillaire (2016/2020) for scattering and the atmosphere transmittance LUT:
//   Init:   compute passes generate the 3D shape (128^3) and detail (32^3)
//           Perlin-Worley noise (with mip chains) and a 2D weather map; a 64x64
//           blue-noise texture is made on the CPU (void and cluster).
//   Render: half-resolution raymarch of the spherical cloud shell against the scene
//           depth (compute), then full-resolution temporal reprojection with
//           neighbourhood clamping (compute).
//   Composite: blends the accumulated clouds over the scene colour (render pass).
// Runs after the game's RenderScene, so opaque world geometry is in the depth buffer.
// Sun transmittance, sky ambient and aerial perspective come from the atmosphere's
// LUTs (webgpu/atmosphere.h, initialised first). Without the atmosphere drawn, and
// at night, ambient and haze fall back to the game's own fog colour and distance.
// GTA-specific: Z is up.
#include <webgpu/webgpu_cpp.h>

#include "game_data.h"

namespace agpu::wgb::clouds {

struct Frame {
  float viewProj[16];     // D3D row-vector order: clip = world * viewProj
  float invViewProj[16];  // world = clip * invViewProj
  float camera[3];        // world position (m)
  float fogColor[3];      // D3DRS_FOGCOLOR (the time-of-day horizon colour), 0..1
  float fogEnd;           // D3DRS_FOGEND (m)
  game::Data game;
  // The atmosphere (webgpu/atmosphere.h) is drawn: use its sky ambient and aerial
  // perspective by day. Its LUTs are used for sun transmittance either way.
  bool atmosphere = false;
};

// Creates the pipelines and generates the noise textures (submitted on `queue`).
// `target`: format of the colour target Composite blends into. Wrap in a validation
// error scope and call Shutdown() if it reports an error.
bool Init(const wgpu::Device& device, const wgpu::Queue& queue, wgpu::TextureFormat target);
void Shutdown();
// Drops the temporal history (camera cuts, mode changes).
void ResetHistory();
// Tests: the generated 3D shape and detail noise textures.
wgpu::Texture ShapeNoise();
wgpu::Texture DetailNoise();
// Encodes the raymarch and reprojection compute passes. `sceneDepth`: depth-only view
// of the scene depth buffer (`width` x `height`), not attached while this runs.
void Render(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::CommandEncoder& encoder,
            const Frame& frame, uint32_t width, uint32_t height, const wgpu::TextureView& sceneDepth);
// Encodes a render pass that blends the clouds into `target` (loaded, no depth).
void Composite(const wgpu::Device& device, const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
               const wgpu::TextureView& sceneDepth);

}  // namespace agpu::wgb::clouds
