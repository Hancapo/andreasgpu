# AndreasGPU renderer design

Goal: replace D3D9 in vanilla `gta_sa.exe` 1.0 US with a WebGPU (Dawn) renderer, keeping the
exe untouched on disk. Everything lives in our `d3d9.dll`.

## What the game needs (measured, see `andreasgpu_logs/` from the shim)

- One pure HAL device, single-threaded. Main back buffer A8R8G8B8, depth D24S8.
- Almost all world rendering is **fixed-function** with `DrawIndexedPrimitive` (tri strips/lists):
  - buildings (`0x5D66F6`): 2 streams (D3DCOLOR in stream 0, pos+uv in stream 1), lighting on with
    vertex colour as emissive, linear table fog, alpha test GREATER with refs 100/140;
  - vehicles (`0x5D9EF0`): pos/normal/uv(/uv2), 1-2 directional lights, specular;
  - stencil shadow volumes (`0x80E70D`): decl pos+colour, stencil INCR/DECR on z-fail, colour writes
    via blend ZERO/ONE.
- Texture stage combiners seen: MODULATE, SELECTARG2, MULTIPLYADD, BLENDFACTORALPHA; texcoord
  generation CAMERASPACENORMAL (env maps); texture transform on stage 1.
- Programmable: 6 `ps_1_1` (MatFX) and RW's generated `vs_2_x` shaders (skinning, lights, fog),
  ~12 variants.
- Textures: DXT1, DXT3, A8R8G8B8, X8R8G8B8; render targets X8R8G8B8 64x64/128x128 (reflections /
  shadows) plus the back buffer; `StretchRect` back buffer -> 4096x2048 X8R8G8B8 (post effects).
- `TestCooperativeLevel`, `SetGammaRamp`, `EvictManagedResources` must keep working.

## Architecture

```
gta_sa.exe ──► DeviceProxy (IDirect3DDevice9) ──► Backend
                                                   ├─ D3D9 passthrough (today)
                                                   └─ WebGPU backend (Dawn, D3D12)
```

1. **Resource proxies.** IDirect3DTexture9 / Surface9 / VertexBuffer9 / IndexBuffer9 /
   VertexDeclaration9 / shaders get proxy objects so Lock/Unlock data reaches the backend.
2. **State model.** One `DeviceState` (render states, stage/sampler states, transforms, lights,
   material, bindings), the same one the recorder already keeps.
3. **Pipeline cache.** A draw's state key (already defined in `recorder.cpp`) selects a
   `wgpu::RenderPipeline` plus generated WGSL:
   - fixed-function vertex stage (transform, lighting, texgen, fog factor),
   - fixed-function fragment stage (texture stage combiners, alpha test via `discard`, fog),
   - or translated SM1/SM2 bytecode for the RW vertex shaders / MatFX pixel shaders.
4. **Dynamic data.** Per-draw uniforms (matrices, lights, material, fog, TFACTOR, alpha ref) in a
   ring buffer with dynamic offsets; `*UP` draws and triangle fans go through a transient ring.
5. **Presentation.** `wgpu::Surface` on the game HWND; borderless as today.

## Development path

1. Build Dawn for x86 (the game is 32-bit) as `webgpu_dawn.dll`, load it inside the game.
2. **Mirror mode**: keep the real D3D9 device rendering, replay every call into the WebGPU backend
   and show it in a second window. Compare frames until they match.
3. Switch: the backend owns presentation and the real D3D9 device is no longer created.
