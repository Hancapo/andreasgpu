// Addresses (gta_sa.exe 1.0 US), each confirmed from the code that uses it:
//   0xB70153 / 0xB70152 u8, 0xB70150 u16  clock hours / minutes / seconds (CClock update 0x52CF10)
//   0xB7CB84 u32  game time in ms; 0xB7CB5C f32 time step; 0xB7CB4C u32 frame counter (CTimer)
//   0xC81320 / 0xC8131C s16  old / new weather; 0xC8130C f32 blend (CWeather::Update 0x72B850)
//   0xC81324 f32  rain (read by the rain streaks 0x72AF70 and post effects 0x7046E0)
//   0xC81308 f32  wet roads (read by the wet-road corona reflections 0x6FB630)
//   0xC812F0 f32  wind (blended from the per-weather table 0x8D5E50; read by plants, clouds)
//   0xC8132C f32  underwater amount (CWeather::Update skips rain when it is > 0)
//   0xB72914 s32  current area / interior (0 outside)
//   0xB7CA50 f32[3][]  vector to the sun, indexed by the timecycle slot at 0xB79FD0
//                 (realtime shadows 0x707CA0 read it the same way)
//   0xB6F03C ptr  TheCamera's matrix (position at +48), else its placement at 0xB6F02C
//                 (the same fallback CWeather::Update uses)
#include "game_data.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "patch.h"

namespace agpu::game {
namespace {

template <typename T>
T At(uintptr_t address) {
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(T));
  return v;
}

}  // namespace

bool Read(Data* out) {
  std::memset(out, 0, sizeof(*out));
  static const bool supported = IsSupportedBuild();
  if (!supported) return false;

  const float seconds = static_cast<float>(At<uint16_t>(0xB70150) & 0xFF);
  out->time[0] = At<uint8_t>(0xB70153) + At<uint8_t>(0xB70152) / 60.0f + seconds / 3600.0f;
  out->time[1] = At<uint32_t>(0xB7CB84) / 1000.0f;
  out->time[2] = At<float>(0xB7CB5C);
  out->time[3] = static_cast<float>(At<uint32_t>(0xB7CB4C) % 16777216u);  // exact in a float

  out->weather[0] = At<int16_t>(0xC81320);
  out->weather[1] = At<int16_t>(0xC8131C);
  out->weather[2] = At<float>(0xC8130C);
  out->weather[3] = At<float>(0xC81324);

  out->env[0] = At<float>(0xC81308);
  out->env[1] = At<float>(0xC812F0);
  out->env[2] = At<float>(0xC8132C);
  out->env[3] = static_cast<float>(At<int32_t>(0xB72914));

  const int32_t slot = At<int32_t>(0xB79FD0);
  if (slot >= 0 && slot < 64) {
    float s[3];
    std::memcpy(s, reinterpret_cast<const void*>(0xB7CA50 + 12 * slot), sizeof(s));
    float len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (len > 0) {
      for (int i = 0; i < 3; ++i) out->sun[i] = s[i] / len;
    }
  }

  const uintptr_t matrix = At<uint32_t>(0xB6F03C);
  std::memcpy(out->camera, reinterpret_cast<const void*>(matrix ? matrix + 48 : 0xB6F02C), 12);
  return true;
}

}  // namespace agpu::game
