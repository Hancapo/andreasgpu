#pragma once
// What the game is drawing right now: hooks around the render phases of gta_sa.exe
// 1.0 US (Idle 0x53E920, RenderScene 0x53DF40, CRenderer::RenderOneNonRoad
// 0x553260) keep a current pass and category, which the WebGPU backend reads for
// every draw. This is the base for mods that treat world, peds, vehicles, HUD...
// differently.
#include <cstdint>

namespace agpu::phase {

// Which image the draws end up in.
enum class Pass : uint8_t {
  Main,       // the frame on screen
  Mirror,     // CMirrors' reflection (1024x512 target), re-renders the scene
  ShadowMap,  // CRealTimeShadowManager: ped/vehicle shadow textures
  kCount
};

// What the draws are.
enum class Category : uint8_t {
  Unknown,     // outside any hooked phase
  Sky,         // horizon gradient, clouds
  Building,    // map geometry (CBuilding), fading and underwater entities
  Road,        // CRenderer::RenderRoads
  Vehicle,
  Ped,         // peds and the weapons they hold
  Object,      // CObject / dummies, breakable objects
  Water,
  Shadow,      // stencil and stored (texture) shadows
  Vegetation,  // CPlantMgr (grass, plants)
  Effects,     // coronas, particles, sun reflection, other RenderEffects
  Weather,     // rain streaks
  Reflection,  // the mirror reflection drawn into the frame
  PostFx,      // full-screen post effects (colour filter, night/IR vision, heat haze...)
  Hud,         // 2D: HUD, radar, text, screen fades
  Menu,        // frontend menu
  kCount
};

Pass CurrentPass();
Category CurrentCategory();
const char* Name(Pass pass);
const char* Name(Category category);

// Installs the phase hooks (after the build check).
void InstallHooks();

}  // namespace agpu::phase
