#pragma once
// Aspect-correct rendering for wide screens (gta_sa.exe 1.0 US):
//  - 3D: Hor+. The real back-buffer aspect replaces the game's fixed 4:3 / 16:9
//    (CDraw::CalculateAspectRatio, 0x6FF420) and the horizontal FOV is widened so
//    the vertical FOV matches 4:3 (CDraw::SetFOV, 0x6FF410). Culling reads the
//    same FOV, so it stays consistent. Sprite sizes (0x70CE30/0x71DA00) are
//    corrected per axis.
//  - 2D: HUD, text, radar and menus are drawn for a 4:3 layout stretched to the
//    full width; they are squeezed back into a centred 4:3 area at the Im2D entry
//    points. Full-width draws, render-to-texture, post effects and sprites
//    projected from 3D are left alone.
#include <cstdint>

namespace agpu {

// Code patches; called with the other game hooks (Direct3DCreate9).
void InstallWidescreenPatches();

// Swaps RenderWare's Im2D entry points; needs the started engine (CreateDevice).
void InstallWidescreen2D();

}  // namespace agpu
