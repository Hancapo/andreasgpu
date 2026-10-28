// d3d9.dll proxy. gta_sa.exe delay-loads d3d9.dll for Direct3DCreate9, so a
// d3d9.dll in the game folder is picked up before the system one. Its startup
// DirectX check (0x745650) also loads and frees d3d9.dll once without creating
// anything, so all hooks are installed lazily in Direct3DCreate9.
#include <windows.h>
#include <d3d9.h>

#include "game_hooks.h"
#include "gen/d3d9_proxy.h"
#include "log.h"
#include "patch.h"
#include "recorder.h"

namespace {

HMODULE g_real = nullptr;
bool g_hooked = false;

FARPROC Real(const char* name) {
  if (!g_real) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit process
    lstrcpynW(path + n, L"\\d3d9.dll", MAX_PATH - n);
    g_real = LoadLibraryW(path);
    if (!g_real) return nullptr;
  }
  return GetProcAddress(g_real, name);
}

template <typename Fn>
Fn RealFn(const char* name) {
  return reinterpret_cast<Fn>(Real(name));
}

// The borderless window must be sized in physical pixels. This has to happen
// before the game creates its window; the first load of this dll (the startup
// DirectX check in psInitialize, 0x747420) comes before that.
void MakeProcessDpiAware() {
  using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  auto setContext = reinterpret_cast<SetContextFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
  SetProcessDPIAware();
}

}  // namespace

extern "C" {

IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion) {
  auto create = RealFn<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
  if (!create) return nullptr;
  IDirect3D9* d3d = create(sdkVersion);
  if (!d3d) return nullptr;
  if (!g_hooked) {
    g_hooked = true;
    agpu::LogInit();
    agpu::Logf("[init] Direct3DCreate9(%u); gta_sa.exe build %s\n", sdkVersion,
               agpu::IsSupportedBuild() ? "1.0 US (supported)" : "UNSUPPORTED");
    agpu::InstallGameHooks();
    agpu::LogFlush();
  }
  return new agpu::Direct3D9Proxy(d3d);
}

HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
  auto create = RealFn<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
  return create ? create(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
  auto fn = RealFn<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent");
  return fn ? fn(color, name) : 0;
}

int WINAPI Proxy_D3DPERF_EndEvent() {
  auto fn = RealFn<int(WINAPI*)()>("D3DPERF_EndEvent");
  return fn ? fn() : 0;
}

DWORD WINAPI Proxy_D3DPERF_GetStatus() {
  auto fn = RealFn<DWORD(WINAPI*)()>("D3DPERF_GetStatus");
  return fn ? fn() : 0;
}

BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame() {
  auto fn = RealFn<BOOL(WINAPI*)()>("D3DPERF_QueryRepeatFrame");
  return fn ? fn() : FALSE;
}

void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
  if (auto fn = RealFn<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker")) fn(color, name);
}

void WINAPI Proxy_D3DPERF_SetOptions(DWORD options) {
  if (auto fn = RealFn<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions")) fn(options);
}

void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
  if (auto fn = RealFn<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion")) fn(color, name);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    if (agpu::IsSupportedBuild()) MakeProcessDpiAware();
  } else if (reason == DLL_PROCESS_DETACH && g_hooked) {
    agpu::rec::Dump("exit");
    // On a FreeLibrary (reserved == nullptr) the code our patches jump to is about
    // to be unmapped, so put the original bytes back. At process exit, leave it.
    if (!reserved) {
      agpu::RevertAllPatches();
      if (g_real) FreeLibrary(g_real);
    }
  }
  return TRUE;
}
