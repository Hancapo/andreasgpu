#pragma once
// Game state the renderer can hand to shaders, read once per frame from gta_sa.exe
// 1.0 US globals (addresses checked in the IDA database, see game_data.cpp). The
// layout is the WGSL `Game` uniform (webgpu/ffshader.cpp), binding 10.

namespace agpu::game {

struct Data {
  // x: time of day in hours (0..24, fractional), y: game time in seconds (CTimer,
  // pauses with the game), z: frame time step (1.0 = 1/50 s), w: frame counter.
  float time[4];
  // x: old weather type, y: new weather type, z: blend from old to new (0..1),
  // w: rain (0..1).
  float weather[4];
  // x: wet roads (0..1), y: wind, z: underwater (0..1), w: area (0 outside, else
  // the interior id).
  float env[4];
  // xyz: direction to the sun (world space, unit length), w: 0.
  float sun[4];
  // xyz: camera position (world space), w: 0.
  float camera[4];
};

// False (and `out` zeroed) on builds other than 1.0 US.
bool Read(Data* out);

}  // namespace agpu::game
