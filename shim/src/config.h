#pragma once

namespace agpu {

// Read once from andreasgpu.ini next to this dll:
//   [renderer]
//   present=webgpu   ; what you see in the game window: webgpu or d3d9 (default)
//   mirror=1         ; also show the WebGPU frame in a second window
//   record=1         ; log every draw state / resource (andreasgpu_logs; costs CPU)
//   fps=0            ; hide the FPS counter in the WebGPU output (shown by default)
//   profile=1        ; log a CPU time breakdown of the WebGPU backend every 600 frames
//   compare=30       ; with mirror=1: compare both renderers every 30 s (F10 any time)
struct Config {
  bool presentWebGPU = false;
  bool mirror = false;
  bool record = false;
  bool profile = false;
  bool fps = true;
  unsigned compareSeconds = 0;
  bool webgpu() const { return presentWebGPU || mirror; }
};

const Config& GetConfig();

}  // namespace agpu
