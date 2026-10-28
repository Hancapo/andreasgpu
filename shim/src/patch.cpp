#include "patch.h"

#include <windows.h>

#include <cstring>
#include <vector>

#include "log.h"

namespace agpu {
namespace {

// gta_sa.exe 1.0 US (14,383,616 bytes) PE header values.
constexpr uintptr_t kImageBase = 0x400000;
constexpr DWORD kTimeDateStamp = 0x427101CA;
constexpr DWORD kSizeOfImage = 0x1177000;

struct SavedBytes {
  uint8_t* address;
  std::vector<uint8_t> original;
};
std::vector<SavedBytes> g_saved;

}  // namespace

bool IsSupportedBuild() {
  auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  if (reinterpret_cast<uintptr_t>(base) != kImageBase) return false;
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kTimeDateStamp &&
         nt->OptionalHeader.SizeOfImage == kSizeOfImage;
}

bool WriteProtected(void* address, const void* data, size_t size) {
  DWORD old = 0;
  if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
  auto p = static_cast<uint8_t*>(address);
  g_saved.push_back({p, std::vector<uint8_t>(p, p + size)});
  std::memcpy(p, data, size);
  VirtualProtect(address, size, old, &old);
  FlushInstructionCache(GetCurrentProcess(), address, size);
  return true;
}

bool InstallCallSiteHook(const CallSiteHook& hook) {
  auto site = reinterpret_cast<uint8_t*>(hook.site);
  int32_t rel = 0;
  std::memcpy(&rel, site + 1, 4);
  uintptr_t target = hook.site + 5 + rel;
  if (site[0] != 0xE8 || target != hook.expectedTarget) {
    Logf("[patch] REFUSED %s: site %08X is not `call %08X`\n", hook.name, hook.site, hook.expectedTarget);
    return false;
  }
  if (std::memcmp(reinterpret_cast<const void*>(hook.expectedTarget), hook.guard, hook.guardSize) != 0) {
    Logf("[patch] REFUSED %s: guard bytes differ at %08X\n", hook.name, hook.expectedTarget);
    return false;
  }
  int32_t newRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook.replacement) - (hook.site + 5));
  if (!WriteProtected(site + 1, &newRel, 4)) {
    Logf("[patch] FAILED %s: VirtualProtect\n", hook.name);
    return false;
  }
  Logf("[patch] installed %s at %08X\n", hook.name, hook.site);
  return true;
}

bool InstallBytePatch(const BytePatch& patch) {
  auto address = reinterpret_cast<void*>(patch.address);
  if (std::memcmp(address, patch.expected, patch.size) != 0) {
    Logf("[patch] REFUSED %s: unexpected bytes at %08X\n", patch.name, patch.address);
    return false;
  }
  if (!WriteProtected(address, patch.replacement, patch.size)) {
    Logf("[patch] FAILED %s: VirtualProtect\n", patch.name);
    return false;
  }
  Logf("[patch] installed %s at %08X\n", patch.name, patch.address);
  return true;
}

bool InstallJumpHook(const char* name, uintptr_t address, const uint8_t* guard, size_t guardSize, void* target) {
  if (guardSize < 5 || std::memcmp(reinterpret_cast<const void*>(address), guard, guardSize) != 0) {
    Logf("[patch] REFUSED %s: unexpected bytes at %08X\n", name, address);
    return false;
  }
  uint8_t jump[5] = {0xE9};
  int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (address + 5));
  std::memcpy(jump + 1, &rel, 4);
  if (!WriteProtected(reinterpret_cast<void*>(address), jump, sizeof(jump))) {
    Logf("[patch] FAILED %s: VirtualProtect\n", name);
    return false;
  }
  Logf("[patch] installed %s at %08X\n", name, address);
  return true;
}

void RevertAllPatches() {
  for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
    DWORD old = 0;
    if (!VirtualProtect(it->address, it->original.size(), PAGE_EXECUTE_READWRITE, &old)) continue;
    std::memcpy(it->address, it->original.data(), it->original.size());
    VirtualProtect(it->address, it->original.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), it->address, it->original.size());
  }
  g_saved.clear();
}

}  // namespace agpu
