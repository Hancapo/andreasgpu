#pragma once
// Starting the WebGPU backend and choosing where it presents:
//  - mirror mode: a second window, next to the D3D9 output in the game window;
//  - present mode: the game window itself (the real D3D9 device then renders to a
//    hidden helper window and only serves as a compatibility layer).
#include <windows.h>

namespace agpu::mirror {

// Creates the Dawn device. `presentWindow` = the game window for present mode, or
// nullptr to create the mirror window. Call on the game's render thread (its
// message loop pumps our windows). `vsync` follows the game's present interval.
// Returns false and logs on failure; the game keeps running on D3D9 either way.
bool Start(HWND presentWindow, bool vsync);
bool Active();

// Called from IDirect3DDevice9::Present, after the real Present.
void OnPresent();

}  // namespace agpu::mirror
