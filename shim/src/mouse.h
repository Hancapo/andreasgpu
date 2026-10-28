#pragma once
#include <windows.h>

namespace agpu {

// Borderless mouse fix. The game reads its mouse through DirectInput 8; in our
// windowed device its exclusive mode (used in gameplay) delivers no data at all.
// The mouse is kept non-exclusive (buttons and wheel keep working) and movement
// comes from Raw Input on the game window. Call once per frame (only in the
// supported gta_sa.exe).
void UpdateMouse(HWND gameWindow);

}  // namespace agpu
