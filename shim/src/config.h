#pragma once

namespace agpu {

// Read once from andreasgpu.ini next to this dll:
//   [renderer]
//   present=webgpu   ; what you see in the game window: webgpu or d3d9 (default)
//   mirror=1         ; also show the WebGPU frame in a second window
//   record=1         ; log every draw state / resource (andreasgpu_logs; costs CPU)
struct Config {
  bool presentWebGPU = false;
  bool mirror = false;
  bool record = false;
  bool webgpu() const { return presentWebGPU || mirror; }
};

const Config& GetConfig();

}  // namespace agpu
