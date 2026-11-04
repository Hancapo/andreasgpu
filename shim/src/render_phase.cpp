// Render phase hooks for gta_sa.exe 1.0 US. Every hooked site is a `call rel32` (or
// the tail `jmp rel32` that ends RenderScene) inside Idle (0x53E920), RenderScene (0x53DF40) or the entity loops, redirected to
// a wrapper that sets the current pass or category, calls the original target and
// restores the previous value. Addresses and the guard bytes (the first 8 bytes of
// each target) come from the IDA database.
#include "render_phase.h"

#include <cstddef>
#include <iterator>

#include "log.h"
#include "patch.h"

namespace agpu::phase {
namespace {

Pass g_pass = Pass::Main;
Category g_category = Category::Unknown;

struct CategoryScope {
  Category saved;
  explicit CategoryScope(Category c) : saved(g_category) { g_category = c; }
  ~CategoryScope() { g_category = saved; }
};
struct PassScope {
  Pass saved;
  Category savedCategory;
  explicit PassScope(Pass p) : saved(g_pass), savedCategory(g_category) {
    g_pass = p;
    g_category = Category::Unknown;
  }
  ~PassScope() {
    g_pass = saved;
    g_category = savedCategory;
  }
};

// __cdecl targets. Forwarding four stack dwords passes any arguments the caller
// pushed (the caller cleans them); unused ones are just the caller's stack.
using CdeclFn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);
template <uintptr_t Target, Category C>
uint32_t __cdecl CdeclCategory(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  CategoryScope scope(C);
  return reinterpret_cast<CdeclFn>(Target)(a, b, c, d);
}
template <uintptr_t Target, Pass P>
uint32_t __cdecl CdeclPass(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  PassScope scope(P);
  return reinterpret_cast<CdeclFn>(Target)(a, b, c, d);
}

// __thiscall targets without stack arguments (they end in a plain `retn`; checked
// for every hooked target in IDA): a
// __fastcall wrapper receives `this` in ecx and pops nothing either.
using ThiscallFn = uint32_t(__thiscall*)(void*);
template <uintptr_t Target, Category C>
uint32_t __fastcall ThiscallCategory(void* self, void*) {
  CategoryScope scope(C);
  return reinterpret_cast<ThiscallFn>(Target)(self);
}
// __thiscall targets with one stack argument (`retn 4`); __fastcall pops it too.
using Thiscall1Fn = uint32_t(__thiscall*)(void*, uint32_t);
template <uintptr_t Target, Category C>
uint32_t __fastcall Thiscall1Category(void* self, void*, uint32_t a) {
  CategoryScope scope(C);
  return reinterpret_cast<Thiscall1Fn>(Target)(self, a);
}
template <uintptr_t Target, Pass P>
uint32_t __fastcall ThiscallPass(void* self, void*) {
  PassScope scope(P);
  return reinterpret_cast<ThiscallFn>(Target)(self);
}

// 0x553260 CRenderer::RenderOneNonRoad(CEntity*): the entity type is the low 3 bits
// of byte +0x36 (1 building, 2 vehicle, 3 ped, 4 object, 5 dummy).
constexpr uintptr_t kRenderOneNonRoad = 0x553260;
uint32_t __cdecl Hook_RenderOneNonRoad(const uint8_t* entity) {
  Category c = Category::Building;
  switch (entity ? entity[0x36] & 7 : 0) {
    case 2: c = Category::Vehicle; break;
    case 3: c = Category::Ped; break;
    case 4:
    case 5: c = Category::Object; break;
    default: break;
  }
  CategoryScope scope(c);
  return reinterpret_cast<uint32_t(__cdecl*)(const uint8_t*)>(kRenderOneNonRoad)(entity);
}

struct Site {
  uintptr_t site;    // the `call rel32`
  uintptr_t target;  // its expected target
  uint8_t guard[8];  // the target's first bytes
  void* wrapper;
  const char* name;
};

#define AGPU_CAT(site, target, cat, name, ...) \
  {site, target, {__VA_ARGS__}, reinterpret_cast<void*>(&CdeclCategory<target, Category::cat>), name}
#define AGPU_THIS_CAT(site, target, cat, name, ...) \
  {site, target, {__VA_ARGS__}, reinterpret_cast<void*>(&ThiscallCategory<target, Category::cat>), name}

const Site kSites[] = {
    // Idle 0x53E920.
    {0x53EA0D, 0x706AB0, {0x51, 0x53, 0x57, 0x8B, 0xF9, 0x8A, 0x07, 0x33},
     reinterpret_cast<void*>(&ThiscallPass<0x706AB0, Pass::ShadowMap>), "CRealTimeShadowManager::Update"},
    {0x53EA12, 0x727140, {0x51, 0xA1, 0x24, 0xC7, 0xC7, 0x00, 0x53, 0x33},
     reinterpret_cast<void*>(&CdeclPass<0x727140, Pass::Mirror>), "CMirrors::BeforeMainRender"},
    AGPU_CAT(0x53EA7E, 0x53D7A0, Sky, "horizon and sky polys", 0x51, 0xD9, 0x05, 0x38, 0x50, 0x8D, 0x00, 0xD8),
    AGPU_CAT(0x53EABA, 0x726090, Reflection, "mirror reflection", 0xA1, 0x24, 0xC7, 0xC7, 0x00, 0x83, 0xEC, 0x08),
    AGPU_CAT(0x53EAC4, 0x732F30, Ped, "weapons of peds", 0x83, 0xEC, 0x0C, 0xA1, 0x24, 0x7B, 0xC9, 0x00),
    AGPU_CAT(0x53EAD3, 0x53E170, Effects, "RenderEffects", 0xE8, 0x9B, 0x46, 0x1D, 0x00, 0xE8, 0xC6, 0x24),
    // RenderEffects ends with a tail `jmp` to the post effects.
    AGPU_CAT(0x53E227, 0x7046E0, PostFx, "post effects", 0xA0, 0xCF, 0x02, 0xC4, 0x00, 0x83, 0xEC, 0x10),
    AGPU_CAT(0x53EB12, 0x53E230, Hud, "Render2dStuff", 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x83, 0xEC, 0x18),
    AGPU_THIS_CAT(0x53EB8C, 0x57C290, Menu, "frontend menu", 0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x32, 0x84, 0xC0),
    AGPU_CAT(0x53EB9D, 0x53E600, Hud, "screen fade", 0xA0, 0x49, 0xCB, 0xB7, 0x00, 0x83, 0xEC, 0x18),
    AGPU_CAT(0x53EBA2, 0x58D490, Hud, "HUD after fade", 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x6A, 0x01, 0x6A),
    // FrontendIdle 0x53E770 (menu without a running game).
    AGPU_THIS_CAT(0x53E82D, 0x57C290, Menu, "frontend menu (frontend idle)", 0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x32, 0x84,
                  0xC0),
    AGPU_CAT(0x53E837, 0x53E600, Hud, "screen fade (frontend idle)", 0xA0, 0x49, 0xCB, 0xB7, 0x00, 0x83, 0xEC, 0x18),
    AGPU_CAT(0x53E83C, 0x58D490, Hud, "HUD after fade (frontend idle)", 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x6A, 0x01,
             0x6A),
    // RenderScene 0x53DF40 (also run by the mirror pass).
    AGPU_CAT(0x53DF9B, 0x7178F0, Sky, "sky background", 0xE9, 0xAB, 0xFA, 0xFF, 0xFF, 0x90, 0x90, 0x90),
    AGPU_CAT(0x53DFA0, 0x713950, Sky, "clouds", 0x83, 0xEC, 0x68, 0xE8, 0x48, 0x8B, 0xE2, 0xFF),
    AGPU_CAT(0x53DFD3, 0x553A10, Road, "RenderRoads", 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x57, 0x6A, 0x01),
    AGPU_CAT(0x53DFD8, 0x6FB630, Effects, "corona reflections", 0xD9, 0x05, 0x08, 0x13, 0xC8, 0x00, 0x83, 0xEC),
    AGPU_CAT(0x53DFDD, 0x553AA0, Building, "RenderEverythingBarRoads", 0x51, 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x6A,
             0x01),
    // CBreakManager render (thiscall, one argument: translucent pass flag).
    {0x53DFE9, 0x59E6A0, {0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x53, 0x56, 0x57},
     reinterpret_cast<void*>(&Thiscall1Category<0x59E6A0, Category::Object>), "breakable objects"},
    {0x53E0FE, 0x59E6A0, {0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x53, 0x56, 0x57},
     reinterpret_cast<void*>(&Thiscall1Category<0x59E6A0, Category::Object>), "breakable objects (translucent)"},
    AGPU_CAT(0x53DFEE, 0x553220, Building, "fading underwater entities", 0xE8, 0x4B, 0x2A, 0x1E, 0x00, 0xE8, 0x06,
             0x2B),
    AGPU_CAT(0x53E004, 0x6EF650, Water, "water", 0x83, 0xEC, 0x5C, 0x53, 0x55, 0x56, 0x57, 0xE8),
    AGPU_CAT(0x53E142, 0x6EF650, Water, "water (second call)", 0x83, 0xEC, 0x5C, 0x53, 0x55, 0x56, 0x57, 0xE8),
    AGPU_CAT(0x53E019, 0x5531E0, Building, "fading entities", 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x6A, 0x01, 0x6A),
    AGPU_CAT(0x53E0BE, 0x707F40, Shadow, "permanent shadows update", 0x53, 0xB9, 0x34, 0xA0, 0xC4, 0x00, 0x33, 0xDB),
    AGPU_CAT(0x53E0C3, 0x708300, Shadow, "static shadows", 0xA1, 0x24, 0x7B, 0xC9, 0x00, 0x83, 0xEC, 0x18),
    AGPU_CAT(0x53E0C8, 0x70A960, Shadow, "stored shadows", 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC),
    AGPU_CAT(0x53E103, 0x5DBAE0, Vegetation, "plants", 0x83, 0xEC, 0x78, 0xB9, 0x00, 0xAE, 0xA9, 0x00),
    AGPU_CAT(0x53E121, 0x7154B0, Sky, "clouds from below", 0x83, 0xEC, 0x44, 0x0F, 0xB7, 0x05, 0x04, 0xC5),
    AGPU_CAT(0x53E126, 0x72AF70, Weather, "rain streaks", 0xA0, 0x48, 0xCB, 0xB7, 0x00, 0x83, 0xEC, 0x2C),
    AGPU_CAT(0x53E12B, 0x6FBAA0, Effects, "sun reflection", 0xA1, 0xD0, 0x9F, 0xB7, 0x00, 0x83, 0xEC, 0x60),
    // RenderScene ends with a tail `jmp` to the stencil shadow volumes and darkening quad.
    AGPU_CAT(0x53E159, 0x7113B0, Shadow, "stencil shadows", 0x83, 0xEC, 0x18, 0xB9, 0x00, 0xAE, 0xA9, 0x00),
};

#undef AGPU_CAT
#undef AGPU_THIS_CAT

// RenderOneNonRoad callers: RenderEverythingBarRoads (twice), the RenderEffects
// entity list (0x553D00) and the fading/alpha entity list (0x732B40).
constexpr uintptr_t kRenderOneNonRoadSites[] = {0x553C52, 0x553CB8, 0x553D8F, 0x732C48};
constexpr uint8_t kRenderOneNonRoadGuard[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8A, 0x46, 0x36};

}  // namespace

Pass CurrentPass() { return g_pass; }
Category CurrentCategory() { return g_category; }

const char* Name(Pass pass) {
  switch (pass) {
    case Pass::Main: return "main";
    case Pass::Mirror: return "mirror";
    case Pass::ShadowMap: return "shadowmap";
    default: return "?";
  }
}

const char* Name(Category category) {
  static const char* const kNames[] = {"unknown", "sky",     "building", "road",    "vehicle",
                                       "ped",     "object",  "water",    "shadow",  "vegetation",
                                       "effects", "weather", "reflection", "postfx", "hud", "menu"};
  static_assert(std::size(kNames) == static_cast<size_t>(Category::kCount));
  size_t i = static_cast<size_t>(category);
  return i < std::size(kNames) ? kNames[i] : "?";
}

void InstallHooks() {
  int installed = 0, total = 0;
  for (const Site& s : kSites) {
    ++total;
    installed += InstallCallSiteHook({s.name, s.site, s.target, s.guard, sizeof(s.guard), s.wrapper});
  }
  for (uintptr_t site : kRenderOneNonRoadSites) {
    ++total;
    installed += InstallCallSiteHook({"CRenderer::RenderOneNonRoad", site, kRenderOneNonRoad, kRenderOneNonRoadGuard,
                                      sizeof(kRenderOneNonRoadGuard),
                                      reinterpret_cast<void*>(&Hook_RenderOneNonRoad)});
  }
  Logf("[phase] %d of %d render phase hooks installed\n", installed, total);
}

}  // namespace agpu::phase
