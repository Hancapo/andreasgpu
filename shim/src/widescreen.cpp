#include "widescreen.h"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "display.h"
#include "log.h"
#include "patch.h"

namespace agpu {
namespace {

constexpr float kBaseAspect = 4.0f / 3.0f;
constexpr double kPi = 3.14159265358979323846;

// RsGlobal.maximumWidth/Height: written by CameraSize (0x72FC70) from the camera
// raster, i.e. the back-buffer size.
const int& g_screenWidth = *reinterpret_cast<const int*>(0xC17044);
const int& g_screenHeight = *reinterpret_cast<const int*>(0xC17048);
float& g_aspectRatio = *reinterpret_cast<float*>(0xC3EFA4);  // CDraw::ms_fAspectRatio
float& g_fov = *reinterpret_cast<float*>(0x8D5038);          // CDraw::ms_fFOV (degrees)

float ScreenAspect() {
  if (g_screenWidth <= 0 || g_screenHeight <= 0) return kBaseAspect;
  return static_cast<float>(g_screenWidth) / static_cast<float>(g_screenHeight);
}

// ---------------------------------------------------------------------------
// 3D

// 0x6FF420 CDraw::CalculateAspectRatio: float __cdecl (), result in ST0 and stored
// in ms_fAspectRatio. Vanilla: 16:9 if the menu's widescreen flag (0xBA6793) is
// set, 5:4 if 0xB6F065, else 4:3.
constexpr uintptr_t kCalculateAspectRatio = 0x6FF420;
constexpr uint8_t kCalculateAspectRatioGuard[] = {0xA0, 0x93, 0x67, 0xBA, 0x00};  // mov al, byte_BA6793

float __cdecl Hook_CalculateAspectRatio() {
  g_aspectRatio = ScreenAspect();
  return g_aspectRatio;
}

// 0x6FF410 CDraw::SetFOV: void __cdecl (float degrees). Vanilla stores the value
// through a copy-protection stub at 0x401025 (decoded pointer = 0x8D5038).
constexpr uintptr_t kSetFov = 0x6FF410;
constexpr uint8_t kSetFovGuard[] = {0x8B, 0x44, 0x24, 0x04, 0xE9};  // mov eax,[esp+4]; jmp 0x401025

// CSprite::CalcScreenCoors (0x70CE30, 0x71DA00) scale sprite width/height by
// screen size / z * 70 / ms_fFOV. With a widened FOV and a wider screen that
// shrinks sprites vertically and stretches them horizontally; the `* 70.0`
// constants (flt_858CE0) are redirected to these per-axis factors.
float g_spriteScaleX = 70.0f;
float g_spriteScaleY = 70.0f;
constexpr uintptr_t kSpriteScaleXOperands[] = {0x70CEF8, 0x71DA8D};  // fmul ds:flt_858CE0 (width)
constexpr uintptr_t kSpriteScaleYOperands[] = {0x70CF0A, 0x71DA9F};  // fmul ds:flt_858CE0 (height)
constexpr uint8_t kFlt70Address[] = {0xE0, 0x8C, 0x85, 0x00};

void __cdecl Hook_SetFov(float degrees) {
  float aspect = ScreenAspect();
  float widened = degrees;
  if (aspect > kBaseAspect + 0.01f) {
    double half = std::tan(degrees * kPi / 360.0) * aspect / kBaseAspect;
    widened = static_cast<float>(std::atan(half) * 360.0 / kPi);
  }
  g_fov = widened;
  // Ideal: width/z * 70/fov computed for a 4:3 screen of the same height.
  g_spriteScaleY = 70.0f * widened / degrees;
  g_spriteScaleX = g_spriteScaleY * kBaseAspect / std::fmax(aspect, kBaseAspect);
}

// ---------------------------------------------------------------------------
// 2D

// RwIm2DVertex for the D3D9 driver (matches vertex declaration POSITIONT, COLOR, TEXCOORD).
struct Im2DVertex {
  float x, y, z, rhw;
  uint32_t color;
  float u, v;
};
static_assert(sizeof(Im2DVertex) == 28);

// RwIm2DRenderPrimitive (0x734E90) is `jmp [RwEngineInstance+0x30]`, the indexed
// variant (0x734EA0, behind an obfuscated stub) ends in `jmp [...+0x34]`. The D3D9
// driver functions live in the device table at 0x8E24B8 and are copied there.
using Im2DPrimitiveFn = int(__cdecl*)(int type, Im2DVertex* vertices, int count);
using Im2DIndexedFn = int(__cdecl*)(int type, Im2DVertex* vertices, int count, uint16_t* indices, int indexCount);
constexpr uintptr_t kEngineInstance = 0xC97B24;
constexpr uintptr_t kIm2DPrimitive = 0x7FBB00;
constexpr uintptr_t kIm2DIndexed = 0x7FBD30;
Im2DPrimitiveFn g_im2DPrimitive = nullptr;
Im2DIndexedFn g_im2DIndexed = nullptr;

// Callers whose 2D must keep full-screen coordinates (return addresses):
//  0x700000-0x706FFF: post effects and shadow-camera blits;
//  0x70C000-0x70FFFF: sprites projected from 3D positions (coronas, lights, sun).
bool IsExemptCaller(uintptr_t caller) {
  return (caller >= 0x700000 && caller < 0x707000) || (caller >= 0x70C000 && caller < 0x710000);
}

std::vector<Im2DVertex> g_scratch;
bool g_gamePatched = false;  // only touch game memory in the supported gta_sa.exe

// Returns a squeezed copy of the vertices, or nullptr to draw them unchanged.
Im2DVertex* Squeeze(uintptr_t caller, const Im2DVertex* vertices, int count) {
  if (count <= 0 || IsExemptCaller(caller)) return nullptr;
  UINT width = 0, height = 0;
  if (!IsBackBufferTarget(&width, &height) || !height) return nullptr;
  float scale = kBaseAspect * static_cast<float>(height) / static_cast<float>(width);
  if (scale > 0.99f) return nullptr;
  float minX = vertices[0].x, maxX = vertices[0].x;
  for (int i = 1; i < count; ++i) {
    minX = std::fmin(minX, vertices[i].x);
    maxX = std::fmax(maxX, vertices[i].x);
  }
  if (minX <= 1.0f && maxX >= static_cast<float>(width) - 1.0f) return nullptr;  // full-width
  g_scratch.assign(vertices, vertices + count);
  float center = static_cast<float>(width) * 0.5f;
  for (Im2DVertex& v : g_scratch) v.x = center + (v.x - center) * scale;
  return g_scratch.data();
}

int __cdecl Hook_Im2DPrimitive(int type, Im2DVertex* vertices, int count) {
  Im2DVertex* squeezed = Squeeze(reinterpret_cast<uintptr_t>(_ReturnAddress()), vertices, count);
  return g_im2DPrimitive(type, squeezed ? squeezed : vertices, count);
}

int __cdecl Hook_Im2DIndexed(int type, Im2DVertex* vertices, int count, uint16_t* indices, int indexCount) {
  Im2DVertex* squeezed = Squeeze(reinterpret_cast<uintptr_t>(_ReturnAddress()), vertices, count);
  return g_im2DIndexed(type, squeezed ? squeezed : vertices, count, indices, indexCount);
}

}  // namespace

void InstallWidescreenPatches() {
  g_gamePatched = true;
  InstallJumpHook("CalculateAspectRatio", kCalculateAspectRatio, kCalculateAspectRatioGuard,
                  sizeof(kCalculateAspectRatioGuard), reinterpret_cast<void*>(&Hook_CalculateAspectRatio));
  InstallJumpHook("SetFOV", kSetFov, kSetFovGuard, sizeof(kSetFovGuard), reinterpret_cast<void*>(&Hook_SetFov));
  const float* scaleX = &g_spriteScaleX;
  const float* scaleY = &g_spriteScaleY;
  for (uintptr_t operand : kSpriteScaleXOperands)
    InstallBytePatch({"SpriteScaleX", operand, kFlt70Address, reinterpret_cast<const uint8_t*>(&scaleX), 4});
  for (uintptr_t operand : kSpriteScaleYOperands)
    InstallBytePatch({"SpriteScaleY", operand, kFlt70Address, reinterpret_cast<const uint8_t*>(&scaleY), 4});
}

void InstallWidescreen2D() {
  if (!g_gamePatched || g_im2DPrimitive) return;
  auto engine = *reinterpret_cast<uint8_t**>(kEngineInstance);
  if (!engine) return;
  auto slots = reinterpret_cast<uintptr_t*>(engine + 0x30);
  if (slots[0] != kIm2DPrimitive || slots[1] != kIm2DIndexed) {
    Logf("[patch] REFUSED Im2D squeeze: engine slots are %08X %08X\n", slots[0], slots[1]);
    g_im2DPrimitive = reinterpret_cast<Im2DPrimitiveFn>(1);  // don't retry
    return;
  }
  g_im2DPrimitive = reinterpret_cast<Im2DPrimitiveFn>(slots[0]);
  g_im2DIndexed = reinterpret_cast<Im2DIndexedFn>(slots[1]);
  uintptr_t hooks[2] = {reinterpret_cast<uintptr_t>(&Hook_Im2DPrimitive),
                        reinterpret_cast<uintptr_t>(&Hook_Im2DIndexed)};
  WriteProtected(slots, hooks, sizeof(hooks));
  Logf("[patch] installed Im2D squeeze in engine table at %08X\n", reinterpret_cast<uintptr_t>(slots));
}

}  // namespace agpu
