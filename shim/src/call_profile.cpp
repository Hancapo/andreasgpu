#include "call_profile.h"

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "config.h"
#include "log.h"

namespace agpu {
namespace {

bool g_on = false;
bool g_checked = false;
unsigned long long g_ticks[kCallSlots] = {};
unsigned g_calls[kCallSlots] = {};

bool On() {
  if (!g_checked) {
    g_on = GetConfig().profile;
    g_checked = true;
  }
  return g_on;
}

const char* SlotName(unsigned slot) {
  if (slot == kCallBufferLock) return "VB/IB Lock+Unlock";
  if (slot == kCallTextureLock) return "texture/surface Lock+Unlock";
  return kDeviceCallNames[slot];
}

}  // namespace

DeviceCallScope::DeviceCallScope(DeviceCall call) : slot(static_cast<unsigned>(call)), start(0) {
  CountDeviceCall(call);
  if (On()) start = __rdtsc();
}

DeviceCallScope::DeviceCallScope(unsigned extraSlot) : slot(extraSlot), start(0) {
  if (On()) start = __rdtsc();
}

DeviceCallScope::~DeviceCallScope() {
  if (!start) return;
  g_ticks[slot] += __rdtsc() - start;
  ++g_calls[slot];
}

void ReportDeviceCalls(double ticksPerMs, unsigned frames) {
  unsigned order[kCallSlots];
  for (unsigned i = 0; i < kCallSlots; ++i) order[i] = i;
  std::sort(order, order + kCallSlots, [](unsigned a, unsigned b) { return g_ticks[a] > g_ticks[b]; });
  double total = 0;
  for (unsigned i = 0; i < kCallSlots; ++i) total += g_ticks[i] / ticksPerMs / frames;
  std::string line = "[profile] d3d9 proxy calls (ms/frame, calls/frame), total ";
  char part[96];
  snprintf(part, sizeof(part), "%.2f:", total);
  line += part;
  for (unsigned i = 0; i < 14 && g_ticks[order[i]]; ++i) {
    unsigned s = order[i];
    snprintf(part, sizeof(part), " %s %.2f/%u", SlotName(s), g_ticks[s] / ticksPerMs / frames, g_calls[s] / frames);
    line += part;
  }
  Logf("%s\n", line.c_str());
  std::fill(std::begin(g_ticks), std::end(g_ticks), 0ull);
  std::fill(std::begin(g_calls), std::end(g_calls), 0u);
}

}  // namespace agpu
