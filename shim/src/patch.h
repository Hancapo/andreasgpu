#pragma once
#include <cstddef>
#include <cstdint>

namespace agpu {

// Every patch is pinned to the exact bytes it expects in gta_sa.exe 1.0 US
// (image base 0x400000). On any mismatch the patch is refused and logged, so a
// different build (compact exe, 1.01, Steam...) runs unpatched instead of crashing.

// Redirects one `call rel32` (E8) instruction to `replacement`. The original target
// must be `expectedTarget` and must start with `guard` bytes.
struct CallSiteHook {
  const char* name;
  uintptr_t site;
  uintptr_t expectedTarget;
  const uint8_t* guard;
  size_t guardSize;
  void* replacement;
};

// Replaces `size` bytes at `address`; the current bytes must equal `expected`.
struct BytePatch {
  const char* name;
  uintptr_t address;
  const uint8_t* expected;
  const uint8_t* replacement;
  size_t size;
};

// Checks the host exe header (image base, timestamp, image size).
bool IsSupportedBuild();
bool InstallCallSiteHook(const CallSiteHook& hook);
bool InstallBytePatch(const BytePatch& patch);
// Replaces the start of a function with `jmp target`; `guard` (at least 5 bytes)
// must match what is there now.
bool InstallJumpHook(const char* name, uintptr_t address, const uint8_t* guard, size_t guardSize, void* target);

// Overwrites `size` bytes of code or read-only data, remembering the original
// bytes so RevertAllPatches() can restore them.
bool WriteProtected(void* address, const void* data, size_t size);
void RevertAllPatches();

}  // namespace agpu
