<img src="assets/icon.svg" width="64" height="64" alt="">

# AndreasGPU

Created in [T3 Code](https://t3.codes).

WebGPU (Dawn) renderer for vanilla GTA San Andreas (PC 1.0 US), in progress.

## Step 1: d3d9 logging shim

`shim/` builds a `d3d9.dll` proxy that forwards to the system D3D9 and records what the
game asks of it, so the WebGPU backend can be sized from real data. It also hosts the
in-memory patch framework: every patch is pinned to the exact bytes of gta_sa.exe 1.0 US
and refuses to install on any other build. The exe on disk is never modified.

Build (Visual Studio 2022 Build Tools, 32-bit):

```
cmake -S . -B build -A Win32
cmake --build build --config Release
build\Release\agpu_smoke.exe %CD%\build\shim\Release\d3d9.dll   # smoke test
```

Use: copy `build\shim\Release\d3d9.dll` next to `gta_sa.exe` in a **clean, unmodded**
install (no ASI loader, SkyGfx, modloader or another d3d9.dll), then play. Output goes to
`andreasgpu_logs\` next to the dll:

- `andreasgpu.log`: device creation, caps, patch status, every generated vertex shader descriptor
- `draw_states.txt`: every distinct state combination used by a draw, sorted by draw count, with
  the gta_sa.exe call sites that issued it
- `summary.txt`: primitive totals, device call counts, texture/buffer/render-target formats,
  vertex declarations
- `vsgen_XXXXXXXX.asm`: vertex shader source text from RenderWare's generator (by descriptor)
- `vs_NNN.bin` / `ps_NNN.bin`: shader bytecode; disassemble with `python -I tools\disasm_shaders.py <files>`

The summaries are rewritten every 3600 frames, on device Reset and at exit.

## Configuration (`andreasgpu.ini` next to the dll)

```
[renderer]
present=webgpu   ; what the game window shows: webgpu or d3d9 (default)
mirror=1         ; with present=d3d9: also show the WebGPU frame in a second window
compare=30       ; with mirror=1: compare both renderers every 30 s (F10 any time)
record=1         ; write the draw-state logs above (costs CPU)
fps=0            ; hide the FPS counter drawn over the WebGPU output (shown by default)
profile=1        ; log a CPU time breakdown of the WebGPU backend every 600 frames
```

**Comparison harness:** with `present=d3d9` and `mirror=1` both renderers draw every frame.
F10 (or `compare=N`) reads back that same frame from each and writes
`andreasgpu_logs\compare\NNN_d3d9.png`, `NNN_webgpu.png` and `NNN_diff.png` (error ×4), and logs
the mean error per channel, the share of pixels off by more than 16, PSNR and a 4×4 grid of
mean errors to `andreasgpu.log`. With `present=webgpu`, F10 saves `NNN_webgpu.png` only.

**Frame trace:** every F10 also writes `compare\NNN_draws.txt`: each draw and clear of the next
frame with its outcome (drawn, compiling, skipped), primitive, first vertex position, depth, blend,
alpha-test and stencil state, texture and viewport.

## Display fixes (in the shim)

- **Borderless:** the game's exclusive fullscreen becomes a windowed D3D9 device on a borderless
  window at the primary monitor's top-left. RenderWare still believes it is fullscreen.
- **All resolutions:** the menu's resolution list (0x745AF5) no longer drops 16:9/21:9 modes, and
  the outdated 45 MB video-memory check is skipped.
- **No "Device Selection" dialog** on multi-monitor setups: the game uses the primary adapter and
  the resolution saved from the menu.
- The process is made DPI-aware so window sizes are physical pixels.

- **Widescreen (Hor+):** the 3D view uses the real screen aspect with the vertical FOV of 4:3
  (wider, not stretched); sprite sizes are corrected per axis. HUD, text, radar and menus are
  squeezed back into a centred 4:3 area; full-width draws, render-to-texture, post effects and
  3D-projected sprites are left alone.
- **Mouse:** the cursor is clipped to the game window while it is in the foreground, so it can't
  slide onto another monitor and deactivate the game's DirectInput mouse.

Pick your native resolution once in Options > Display Setup > Resolution; it's saved.

## Dawn (x86)

`tools\build_dawn_x86.cmd` builds `third_party\dawn` (cloned there, not committed) into
`third_party\dawn-build-x86\webgpu_dawn.dll`. Notes:

- Built with **clang-cl** (`-m32`), like Chromium's 32-bit builds. With MSVC x86, Dawn's
  `alignas(8)` `TypedInteger` arguments get two incompatible calling conventions and the D3D
  queue crashes on the first wait.
- Dawn loads its shader compilers from its own folder, and Windows has no x86 DXC, so ship
  `d3dcompiler_47.dll`, `dxcompiler.dll` and `dxil.dll` from the Windows SDK
  (`Redist\D3D\x86`) next to `webgpu_dawn.dll`. CMake copies them for the tests.
- `build\Release\agpu_dawn_smoke.exe [d3d12|d3d11]` checks device, surface, pipeline and present.

## License

MIT, see [LICENSE](LICENSE). Dawn (fetched and built separately into `third_party/`) is
BSD-3-Clause. GTA San Andreas is not included; you need your own copy of the game.
