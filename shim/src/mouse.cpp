// See mouse.h. Evidence (andreasgpu.log, 2026-10-28):
//  - SetCooperativeLevel flags 5 (EXCLUSIVE|FOREGROUND, gameplay; 0x7469A0 called
//    from 0x576B70): every GetDeviceState succeeds with zero movement and no buttons.
//  - DirectInput itself reads the mouse through Raw Input (RIDEV_INPUTSINK on its own
//    hidden window). Raw input allows one mouse registration per process, so the game
//    window and DirectInput can't both have it.
// So the game window owns the raw mouse registration and the whole DIMOUSESTATE the
// game reads (movement, wheel, buttons) is filled from Raw Input.
#include "mouse.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstdint>

#include "log.h"
#include "patch.h"

namespace agpu {
namespace {

constexpr uintptr_t kPlatformGlobals = 0xC17054;  // RsGlobal.ps; mouse device at +0x1C
constexpr DWORD kExclusive = 0x1, kNonExclusive = 0x2;

// IDirectInputDevice8A vtable slots.
constexpr int kGetDeviceState = 9;
constexpr int kSetCooperativeLevel = 13;
constexpr int kGetDeviceInfo = 15;

using GetDeviceStateFn = HRESULT(__stdcall*)(void* self, DWORD size, void* data);
using SetCooperativeLevelFn = HRESULT(__stdcall*)(void* self, HWND window, DWORD flags);
GetDeviceStateFn g_getDeviceState = nullptr;
SetCooperativeLevelFn g_setCooperativeLevel = nullptr;
void** g_hookedVtbl = nullptr;
void* g_mouse = nullptr;
WNDPROC g_gameWndProc = nullptr;
bool g_rawRegistered = false;

// Raw mouse state accumulated since the game last read the mouse.
struct RawMouse {
  long x = 0, y = 0, wheel = 0;
  unsigned held = 0;     // buttons currently down (bit n = button n)
  unsigned pressed = 0;  // buttons that went down since the last read, so short clicks aren't lost
} g_raw;

struct Stats {
  unsigned reads = 0, rawMessages = 0, reRegistered = 0, clicks = 0;
  long x = 0, y = 0, wheel = 0;
} g_stats;

bool IsMouse(void* device) {
  using GetDeviceInfoFn = HRESULT(__stdcall*)(void* self, DIDEVICEINSTANCEA* info);
  auto getInfo = reinterpret_cast<GetDeviceInfoFn>((*reinterpret_cast<void***>(device))[kGetDeviceInfo]);
  DIDEVICEINSTANCEA info{};
  info.dwSize = sizeof(info);
  return SUCCEEDED(getInfo(device, &info)) && GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_MOUSE;
}

HRESULT __stdcall Hook_SetCooperativeLevel(void* self, HWND window, DWORD flags) {
  // Exclusive mode would capture/hide the system cursor through DirectInput's own
  // raw-input registration; the state now comes from the game window instead.
  if ((flags & kExclusive) && IsMouse(self)) flags = (flags & ~kExclusive) | kNonExclusive;
  return g_setCooperativeLevel(self, window, flags);
}

HRESULT __stdcall Hook_GetDeviceState(void* self, DWORD size, void* data) {
  HRESULT hr = g_getDeviceState(self, size, data);
  // The vtable is shared by every DirectInput device; mouse state is DIMOUSESTATE
  // (16 bytes) or DIMOUSESTATE2 (20), joysticks use DIJOYSTATE2 (272).
  bool isMouseState = size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2);
  if (!isMouseState || !g_rawRegistered) return hr;
  if (FAILED(hr)) {
    // Not acquired (e.g. lost focus): report nothing, like DirectInput would.
    g_raw.x = g_raw.y = g_raw.wheel = 0;
    g_raw.pressed = 0;
    return hr;
  }
  auto state = static_cast<DIMOUSESTATE2*>(data);
  state->lX = g_raw.x;
  state->lY = g_raw.y;
  state->lZ = g_raw.wheel;
  unsigned down = g_raw.held | g_raw.pressed;
  int buttons = size == sizeof(DIMOUSESTATE2) ? 8 : 4;
  for (int i = 0; i < buttons; ++i) state->rgbButtons[i] = (down >> i) & 1 ? 0x80 : 0;
  g_raw.x = g_raw.y = g_raw.wheel = 0;
  g_raw.pressed = 0;
  ++g_stats.reads;
  return hr;
}

void OnRawMouse(const RAWMOUSE& m) {
  ++g_stats.rawMessages;
  if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
    g_raw.x += m.lLastX;
    g_raw.y += m.lLastY;
    g_stats.x += m.lLastX;
    g_stats.y += m.lLastY;
  }
  // RI_MOUSE_BUTTON_n_DOWN = 1 << (2n), _UP = 1 << (2n+1), n = 0..4
  for (int n = 0; n < 5; ++n) {
    if (m.usButtonFlags & (1u << (2 * n))) {
      g_raw.held |= 1u << n;
      g_raw.pressed |= 1u << n;
      ++g_stats.clicks;
    }
    if (m.usButtonFlags & (1u << (2 * n + 1))) g_raw.held &= ~(1u << n);
  }
  if (m.usButtonFlags & RI_MOUSE_WHEEL) {
    long delta = static_cast<SHORT>(m.usButtonData);
    g_raw.wheel += delta;
    g_stats.wheel += delta;
  }
}

LRESULT CALLBACK MouseWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INPUT: {
      RAWINPUT raw;
      UINT size = sizeof(raw);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
              static_cast<UINT>(-1) &&
          raw.header.dwType == RIM_TYPEMOUSE)
        OnRawMouse(raw.data.mouse);
      break;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
      if (msg == WM_KILLFOCUS || LOWORD(wp) == WA_INACTIVE) g_raw = RawMouse{};  // no stuck buttons
      break;
  }
  return CallWindowProcA(g_gameWndProc, w, msg, wp, lp);
}

// DirectInput registers raw input for the mouse whenever the game (re)creates its
// device, taking it away from the game window. Put it back.
void EnsureRawInput(HWND gameWindow) {
  UINT count = 0;
  GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
  RAWINPUTDEVICE devices[16];
  count = count > 16 ? 16 : count;
  if (count && GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
    count = 0;
  for (UINT i = 0; i < count; ++i)
    if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02 && devices[i].hwndTarget == gameWindow &&
        devices[i].dwFlags == 0)
      return;
  RAWINPUTDEVICE device{0x01, 0x02, 0, gameWindow};  // generic desktop / mouse
  g_rawRegistered = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
  ++g_stats.reRegistered;
}

template <typename Fn>
void HookSlot(void** vtbl, int index, Fn hook, Fn* orig) {
  *orig = reinterpret_cast<Fn>(vtbl[index]);
  void* replacement = reinterpret_cast<void*>(hook);
  WriteProtected(&vtbl[index], &replacement, sizeof(replacement));
}

}  // namespace

void UpdateMouse(HWND gameWindow) {
  if (!gameWindow) return;
  if (!g_gameWndProc) {
    g_gameWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MouseWndProc)));
    EnsureRawInput(gameWindow);
    Logf("[mouse] raw input on game window %p: %s\n", gameWindow, g_rawRegistered ? "ok" : "FAILED");
  }
  static unsigned frame = 0;
  if (++frame % 30 == 0) EnsureRawInput(gameWindow);

  auto ps = *reinterpret_cast<uint8_t**>(kPlatformGlobals);
  if (!ps) return;
  void* mouse = *reinterpret_cast<void**>(ps + 0x1C);
  if (mouse && mouse != g_mouse) {
    g_mouse = mouse;
    void** vtbl = *reinterpret_cast<void***>(mouse);
    if (!g_hookedVtbl) {
      g_hookedVtbl = vtbl;
      HookSlot(vtbl, kGetDeviceState, &Hook_GetDeviceState, &g_getDeviceState);
      HookSlot(vtbl, kSetCooperativeLevel, &Hook_SetCooperativeLevel, &g_setCooperativeLevel);
      Logf("[mouse] hooked DirectInput mouse %p (vtbl %p)\n", mouse, vtbl);
    } else if (vtbl != g_hookedVtbl) {
      Logf("[mouse] mouse device %p has a different vtable %p; not hooked\n", mouse, vtbl);
    }
  }

  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (now - last < 5000) return;
  last = now;
  Logf("[mouse] last 5 s: reads=%u raw msgs=%u d=(%ld,%ld) wheel=%ld clicks=%u re-registered=%u fg=%d\n",
       g_stats.reads, g_stats.rawMessages, g_stats.x, g_stats.y, g_stats.wheel, g_stats.clicks,
       g_stats.reRegistered, GetForegroundWindow() == gameWindow);
  LogFlush();
  g_stats = Stats{};
}

}  // namespace agpu
