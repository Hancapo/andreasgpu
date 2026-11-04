#pragma once

#include <string>

namespace agpu {

// Read once from andreasgpu.ini next to this dll:
//   [renderer]
//   present=webgpu   ; what you see in the game window: webgpu or d3d9 (default)
//   mirror=1         ; also show the WebGPU frame in a second window
//   record=1         ; log every draw state / resource (andreasgpu_logs; costs CPU)
//   fps=0            ; hide the FPS counter in the WebGPU output (shown by default)
//   validation=1     ; keep Dawn validation and shader bounds checks (slower; for debugging)
//   dawn_toggles=a,b ; exact Dawn device toggles (overrides validation; for testing)
//   profile=1        ; log a CPU time breakdown of the WebGPU backend every 600 frames
//   compare=30       ; with mirror=1: compare both renderers every 30 s (F10 any time)
//   clouds=1         ; WebGPU volumetric clouds: 0 off, 1 over the game's clouds, 2 instead
//                    ; of them (F9 cycles the modes in game)
//   atmosphere=1     ; WebGPU physical sky and aerial perspective (F8 toggles in game)
struct Config {
  bool presentWebGPU = false;
  bool mirror = false;
  bool record = false;
  bool profile = false;
  bool validation = false;
  std::string dawnToggles;
  bool fps = true;
  unsigned compareSeconds = 0;
  int clouds = 0;
  bool atmosphere = false;
  bool webgpu() const { return presentWebGPU || mirror; }
};

const Config& GetConfig();

}  // namespace agpu
