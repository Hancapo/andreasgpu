#pragma once
// Borderless: the game only ever asks for exclusive fullscreen. D3D gets a windowed
// device instead and the game window becomes a borderless popup at the monitor's
// top-left, sized to the back buffer. RenderWare keeps believing it is fullscreen
// because it reads its own copy of the present parameters (0xC9C040, Windowed at
// +0x20) rather than ours; after a resolution change it resizes the window to the
// new mode itself (RwD3D9ChangeVideoMode, 0x7F8640). At the monitor's native
// resolution the window covers the whole screen.
#include <windows.h>
#include <d3d9.h>

namespace agpu {

// Parameters for the real CreateDevice/Reset; restyles `window` if needed.
D3DPRESENT_PARAMETERS BorderlessPresentParams(const D3DPRESENT_PARAMETERS& game, HWND window);

// Copies back what D3D filled in, keeping the game's Windowed/refresh-rate fields.
void CopyBackPresentParams(D3DPRESENT_PARAMETERS* game, const D3DPRESENT_PARAMETERS& used);

// Render-target tracking: after CreateDevice/Reset the target is the back buffer.
void OnBackBuffer(UINT width, UINT height);
void OnSetRenderTarget0(IDirect3DSurface9* surface);
// True while drawing to a surface the size of the back buffer; returns its size.
bool IsBackBufferTarget(UINT* width, UINT* height);

// In exclusive fullscreen the cursor could not leave the game; borderless on a
// multi-monitor desktop it can, and a click outside deactivates the game and its
// DirectInput mouse. Clips the cursor to the game window while it is foreground.
void KeepCursorInWindow(HWND window);

}  // namespace agpu
