#pragma once
// Same-frame comparison of the two renderers (mirror mode: present=d3d9, mirror=1).
// F10, or every `compare` seconds from andreasgpu.ini, captures one frame from the
// real D3D9 back buffer and from the WebGPU target, writes
// andreasgpu_logs\compare\<n>_d3d9.png / _webgpu.png / _diff.png and logs the error.
#include <windows.h>
#include <d3d9.h>

namespace agpu::compare {

// Around the real Present: Before reads the D3D9 back buffer when a capture is
// due; After (once the WebGPU frame was submitted) compares and writes the files.
void BeforePresent(IDirect3DDevice9* real, HWND window);
void AfterPresent();

}  // namespace agpu::compare
