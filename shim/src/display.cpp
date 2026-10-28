#include "display.h"

#include "log.h"

namespace agpu {
namespace {

void MakeBorderless(HWND w, UINT width, UINT height) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
  if (!width || !height) {
    width = mi.rcMonitor.right - mi.rcMonitor.left;
    height = mi.rcMonitor.bottom - mi.rcMonitor.top;
  }
  LONG style = GetWindowLongW(w, GWL_STYLE);
  SetWindowLongW(w, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
  LONG exStyle = GetWindowLongW(w, GWL_EXSTYLE);
  SetWindowLongW(w, GWL_EXSTYLE,
                 exStyle & ~(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                             WS_EX_STATICEDGE));
  SetWindowPos(w, HWND_NOTOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, width, height,
               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  Logf("[display] borderless %ux%u at %ld,%ld (monitor %ldx%ld)\n", width, height, mi.rcMonitor.left,
       mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
}

}  // namespace

D3DPRESENT_PARAMETERS BorderlessPresentParams(const D3DPRESENT_PARAMETERS& game, HWND window) {
  D3DPRESENT_PARAMETERS p = game;
  if (!game.Windowed) {
    p.Windowed = TRUE;
    p.FullScreen_RefreshRateInHz = 0;
    if (window) MakeBorderless(window, game.BackBufferWidth, game.BackBufferHeight);
  }
  return p;
}

namespace {
UINT g_backBufferWidth = 0, g_backBufferHeight = 0;
UINT g_targetWidth = 0, g_targetHeight = 0;
bool g_cursorClipped = false;
}  // namespace

void OnBackBuffer(UINT width, UINT height) {
  g_backBufferWidth = g_targetWidth = width;
  g_backBufferHeight = g_targetHeight = height;
}

void OnSetRenderTarget0(IDirect3DSurface9* surface) {
  D3DSURFACE_DESC desc{};
  if (surface && SUCCEEDED(surface->GetDesc(&desc))) {
    g_targetWidth = desc.Width;
    g_targetHeight = desc.Height;
  }
}

bool IsBackBufferTarget(UINT* width, UINT* height) {
  *width = g_targetWidth;
  *height = g_targetHeight;
  return g_targetWidth == g_backBufferWidth && g_targetHeight == g_backBufferHeight && g_targetWidth;
}

void KeepCursorInWindow(HWND window) {
  if (!window) return;
  if (GetForegroundWindow() == window && !IsIconic(window)) {
    RECT rc;
    GetClientRect(window, &rc);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&rc), 2);
    RECT current;
    GetClipCursor(&current);
    if (!EqualRect(&rc, &current)) ClipCursor(&rc);
    g_cursorClipped = true;
  } else if (g_cursorClipped) {
    ClipCursor(nullptr);
    g_cursorClipped = false;
  }
}

void CopyBackPresentParams(D3DPRESENT_PARAMETERS* game, const D3DPRESENT_PARAMETERS& used) {
  BOOL windowed = game->Windowed;
  UINT refresh = game->FullScreen_RefreshRateInHz;
  *game = used;
  game->Windowed = windowed;
  game->FullScreen_RefreshRateInHz = refresh;
}

}  // namespace agpu
