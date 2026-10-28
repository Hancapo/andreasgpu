#pragma once
// Time spent in each IDirect3DDevice9 proxy method and in resource lock/unlock
// (profile=1). Device methods include whatever they forward to (the real device,
// the recorder, the WebGPU backend). Logged with the backend profile.
#include "gen/d3d9_proxy.h"

namespace agpu {

// Extra slots after the device methods.
constexpr unsigned kCallBufferLock = static_cast<unsigned>(DeviceCall::Count);
constexpr unsigned kCallTextureLock = kCallBufferLock + 1;
constexpr unsigned kCallSlots = kCallTextureLock + 1;

// Logs the most expensive slots as ms and calls per frame, then resets.
void ReportDeviceCalls(double ticksPerMs, unsigned frames);

}  // namespace agpu
