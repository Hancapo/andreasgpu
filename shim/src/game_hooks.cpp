// Hooks inside gta_sa.exe 1.0 US. Addresses and signatures come from the IDA
// database (names prefixed agpu_), see analysis/feasibility.md.
#include "game_hooks.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.h"
#include "patch.h"
#include "widescreen.h"

namespace agpu {
namespace {

// 0x75F0B0 agpu_RwD3D9GenerateVertexShader: int __cdecl (u8 *desc, u8 *outInfo).
// desc is a packed u32 (the caller's cache is keyed by *(u32*)desc); outInfo is 8
// bytes the cache stores alongside the shader. Returns IDirect3DVertexShader9*.
using GenerateVertexShaderFn = int(__cdecl*)(uint8_t* desc, uint8_t* outInfo);
constexpr uintptr_t kGenerateVertexShader = 0x75F0B0;
constexpr uintptr_t kGenerateVertexShaderCall = 0x75EFA0;  // in agpu_RwD3D9GetCachedVertexShader
constexpr uint8_t kGenerateVertexShaderGuard[] = {0x83, 0xEC, 0x38, 0x53, 0x8B, 0x5C, 0x24, 0x40,
                                                  0x33, 0xC0, 0x55, 0x8A, 0x53, 0x02};

// 0x7652AE: statically linked D3DXAssembleShader, __stdcall, 7 args (retn 1Ch).
using AssembleShaderFn = HRESULT(__stdcall*)(const char* src, UINT srcLen, const void* defines,
                                             void* include, DWORD flags, void** shader, void** errors);
constexpr uintptr_t kAssembleShader = 0x7652AE;
constexpr uintptr_t kAssembleShaderCall = 0x760C66;  // in agpu_RwD3D9GenerateVertexShader
constexpr uint8_t kAssembleShaderGuard[] = {0x55, 0x8D, 0x6C, 0x24, 0xA4, 0x81, 0xEC,
                                            0x10, 0x01, 0x00, 0x00, 0x56, 0x57};

// 0x745AF5 builds the menu's resolution list from RW's video modes. It drops any
// mode whose height/width isn't exactly 0.75, 0.8, 0.6 or 0.625, so 16:9 and wider
// never show up. Replace the first `fld` of the aspect compares with a jump to the
// accept path (0x745BCB); the compares only push/pop the FPU stack, so skipping
// them leaves it balanced.
constexpr uintptr_t kAspectFilter = 0x745B87;
constexpr uint8_t kAspectFilterOriginal[] = {0xD9, 0x44, 0x24, 0x08};  // fld [esp+8]
constexpr uint8_t kAspectFilterPatched[] = {0xEB, 0x42, 0x90, 0x90};   // jmp 0x745BCB

// Same function: modes are also dropped unless DirectDraw's video memory total
// minus the mode's framebuffer size is above 45 MB (unsigned compare). Meaningless
// on current GPUs and can wrap, so the `jbe reject` becomes NOPs.
constexpr uintptr_t kVideoMemoryCheck = 0x745BFC;
constexpr uint8_t kVideoMemoryCheckOriginal[] = {0x76, 0x3A};  // jbe 0x745C38
constexpr uint8_t kVideoMemoryCheckPatched[] = {0x90, 0x90};

// psSelectDevice (0x746190) opens a modal "Device Selection" dialog whenever there
// is more than one adapter (any multi-monitor setup), and that dialog only lists
// the old aspect ratios. Always take the single-adapter path: the current (primary)
// adapter and the resolution saved from the menu.
constexpr uintptr_t kDeviceDialog = 0x746225;
constexpr uint8_t kDeviceDialogOriginal[] = {0x7E, 0x4C};  // jle 0x746273
constexpr uint8_t kDeviceDialogPatched[] = {0xEB, 0x4C};   // jmp 0x746273

bool g_inGenerator = false;
uint32_t g_generatorDesc = 0;

int __cdecl Hook_GenerateVertexShader(uint8_t* desc, uint8_t* outInfo) {
  std::memcpy(&g_generatorDesc, desc, 4);
  g_inGenerator = true;
  int shader = reinterpret_cast<GenerateVertexShaderFn>(kGenerateVertexShader)(desc, outInfo);
  g_inGenerator = false;
  uint32_t info[2];
  std::memcpy(info, outInfo, sizeof(info));
  Logf("[vsgen] desc=%08X -> vs=%08X outInfo=%08X %08X\n", g_generatorDesc, shader, info[0], info[1]);
  return shader;
}

HRESULT __stdcall Hook_AssembleShader(const char* src, UINT srcLen, const void* defines, void* include,
                                      DWORD flags, void** shader, void** errors) {
  if (g_inGenerator) {
    wchar_t name[64];
    swprintf(name, 64, L"vsgen_%08X.asm", g_generatorDesc);
    WriteLogFile(name, src, srcLen);
  }
  HRESULT hr = reinterpret_cast<AssembleShaderFn>(kAssembleShader)(src, srcLen, defines, include, flags,
                                                                    shader, errors);
  if (FAILED(hr)) Logf("[vsgen] D3DXAssembleShader failed hr=%08X desc=%08X\n", hr, g_generatorDesc);
  return hr;
}

}  // namespace

bool InVertexShaderGenerator(uint32_t* desc) {
  if (g_inGenerator) *desc = g_generatorDesc;
  return g_inGenerator;
}

void InstallGameHooks() {
  if (!IsSupportedBuild()) {
    Logf("[patch] gta_sa.exe is not the supported 1.0 US build; no game patches applied\n");
    return;
  }
  InstallCallSiteHook({"GenerateVertexShader", kGenerateVertexShaderCall, kGenerateVertexShader,
                       kGenerateVertexShaderGuard, sizeof(kGenerateVertexShaderGuard),
                       reinterpret_cast<void*>(&Hook_GenerateVertexShader)});
  InstallCallSiteHook({"D3DXAssembleShader", kAssembleShaderCall, kAssembleShader, kAssembleShaderGuard,
                       sizeof(kAssembleShaderGuard), reinterpret_cast<void*>(&Hook_AssembleShader)});
  InstallBytePatch({"AllowAllAspectRatios", kAspectFilter, kAspectFilterOriginal, kAspectFilterPatched,
                    sizeof(kAspectFilterOriginal)});
  InstallBytePatch({"SkipVideoMemoryCheck", kVideoMemoryCheck, kVideoMemoryCheckOriginal,
                    kVideoMemoryCheckPatched, sizeof(kVideoMemoryCheckOriginal)});
  InstallBytePatch({"SkipDeviceSelectionDialog", kDeviceDialog, kDeviceDialogOriginal, kDeviceDialogPatched,
                    sizeof(kDeviceDialogOriginal)});
  InstallWidescreenPatches();
}

}  // namespace agpu
