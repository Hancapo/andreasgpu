#pragma once
// Sky and atmosphere after Hillaire, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" (EGSR 2020): Rayleigh, Mie and ozone in a 100 km
// shell over a 6360 km planet, with
//   transmittance LUT 256x64 and multiple-scattering LUT 32x32 (once, at Init),
//   sky-view LUT 192x108 and aerial-perspective volume 32x32x32 (every frame).
// Apply() then draws the sky where the scene shows sky and adds aerial perspective
// over the scene geometry; the clouds (webgpu/clouds.h) sample the same LUTs for
// sun transmittance, sky ambient and aerial perspective.
// GTA-specific: Z is up (planet centre below the world origin); the model has no moon
// or stars, so the sky blends back to the game's own night sky as the sun sets.
#include <webgpu/webgpu_cpp.h>

#include "game_data.h"

namespace agpu::wgb::atmosphere {

struct Frame {
  float invViewProj[16];  // D3D row-vector order: world = clip * invViewProj
  float camera[3];        // world position (m)
  game::Data game;
};

// What the clouds read (valid after Init).
struct Views {
  wgpu::TextureView transmittance, multiScattering, skyView, aerialPerspective;
};

// Sun illuminance (scene units) and the exposure shared by sky and clouds
// (displayed colour = 1 - exp(-radiance * exposure)).
constexpr float kSunIlluminance = 14.0f;
constexpr float kExposure = 1.7f;
constexpr float kAerialPerspectiveDistance = 64000.0f;  // m, squared slice distribution

// WGSL shared with the clouds: atmosphere constants, medium, phase functions, LUT
// mappings and the spec's bridge functions (sampleSkyAmbientIrradiance,
// sampleAerialPerspective, atmoTransmittance). Lengths in km.
const char* CommonWgsl();

// Creates the LUTs and pipelines and computes the static LUTs (submitted on `queue`).
// Wrap in a validation error scope and call Shutdown() if it reports an error.
bool Init(const wgpu::Device& device, const wgpu::Queue& queue, wgpu::TextureFormat target);
void Shutdown();
const Views& GetViews();
// Day factor for the given direction to the sun: 1 by day, 0 at night (game sky).
float DayFactor(const float sun[3]);
// Encodes the per-frame LUTs (sky view, aerial perspective).
void Update(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::CommandEncoder& encoder,
            const Frame& frame, uint32_t width, uint32_t height);
// Encodes a render pass over `target`: sky where `sceneDepth` is sky, aerial
// perspective elsewhere.
void Apply(const wgpu::Device& device, const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
           const wgpu::TextureView& sceneDepth);

}  // namespace agpu::wgb::atmosphere
