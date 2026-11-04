#pragma once
// WebGPU (Dawn) backend. In mirror mode it receives every relevant D3D9 call
// alongside the real device and renders the frame itself into the mirror window.
// Fixed-function draws are translated (webgpu/ffshader.h); draws that use vertex
// or pixel shaders are skipped for now.
#include <windows.h>
#include <d3d9.h>

#include <vector>

namespace agpu {
class ResourceProxy;
}

namespace agpu::wgb {

bool Active();

// After CreateDevice / Reset: D3D state is back to defaults. `depthFormat` is the
// D3D9 auto depth-stencil format: without stencil bits (D16, the game's choice in
// 16-bit video modes) D3D9 passes every stencil test, and so does the backend.
void ResetDevice(UINT backBufferWidth, UINT backBufferHeight, BOOL autoDepthStencil, D3DFORMAT depthFormat);

// Resources (proxies from resources.h).
void TextureCreated(ResourceProxy* texture, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format);
void TextureWritten(ResourceProxy* texture, UINT level, const D3DLOCKED_RECT& locked, const RECT* rect);
void BufferCreated(ResourceProxy* buffer, UINT length, DWORD usage, D3DPOOL pool, bool index, D3DFORMAT indexFormat);
// Dynamic and index buffers keep a CPU shadow; Lock hands the game a pointer into
// it (cached memory) instead of the D3D9 buffer, which is often write-combined and
// very slow to read back. Returns nullptr when the buffer has no shadow.
void* BufferShadow(ResourceProxy* buffer, UINT offset);
// After Unlock. `data` points at the written bytes (the shadow or the D3D9 memory);
// `forward`, if not null, is the D3D9 buffer memory that must receive them too.
void BufferWritten(ResourceProxy* buffer, UINT offset, UINT size, const void* data, void* forward);
void ResourceDestroyed(ResourceProxy* resource);
void VertexDeclarationCreated(const void* decl, const D3DVERTEXELEMENT9* elements);

// State.
void SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
void SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value);
void SetTexture(DWORD stage, ResourceProxy* texture);
void SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
void SetLight(DWORD index, const D3DLIGHT9* light);
void LightEnable(DWORD index, BOOL enable);
void SetMaterial(const D3DMATERIAL9* material);
void SetViewport(const D3DVIEWPORT9* viewport);
void SetStreamSource(UINT stream, ResourceProxy* buffer, UINT offset, UINT stride);
void SetIndices(ResourceProxy* buffer);
void SetVertexDeclaration(const void* decl);
void SetFVF(DWORD fvf);
void ShaderCreated(const void* shader, const DWORD* code, bool vertex);
void SetVertexShader(const void* shader);
void SetPixelShader(const void* shader);
void SetVertexShaderConstantF(UINT start, const float* data, UINT count);
void SetPixelShaderConstantF(UINT start, const float* data, UINT count);
// `surface` is the game-visible proxy (nullptr unbinds).
void SetRenderTarget(DWORD index, IDirect3DSurface9* surface);
void SetDepthStencilSurface(IDirect3DSurface9* surface);

// Commands.
void Clear(DWORD flags, D3DCOLOR color, float z, DWORD stencil);
void DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount);
void DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex,
                          UINT primitiveCount);
void StretchRect(IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* dest, const RECT* destRect);
void Present();
// The game's gamma ramp (brightness setting). D3D9 applies it at scanout and only in
// fullscreen, so with the borderless window it is lost; the WebGPU blit applies it.
void SetGammaRamp(const D3DGAMMARAMP* ramp);

// The game is about to play an FMV in a DirectShow child window of the game window:
// hide the WebGPU surface until the next presented frame.
void MovieStarting();

// Comparison harness: the next Present also reads the WebGPU back buffer back.
void RequestCapture();
bool TakeCapture(std::vector<uint8_t>* bgra, UINT* width, UINT* height);
// Comparison experiments: groups of draws the WebGPU renderer leaves out (from the
// next frame on) so a capture shows whether they cause a difference.
enum : unsigned {
  kSkipStencilQuad = 1,       // pretransformed draws with stencil test (shadow darkening)
  kSkipStencilVolumes = 2,    // 3D draws with stencil test (shadow volumes)
  kSkipShadowTextures = 4,    // 3D draws textured with a render target (realtime shadows)
  kSkipAdditive = 8,          // 3D draws blending with DESTBLEND=ONE (light pools, glows)
  kSkipDecals = 16,           // 3D blended draws without depth writes or stencil (decals, shadows)
  kSkipFades = 64,            // 3D blends whose stage 1 is MODULATE(CURRENT, TFACTOR) (distance fades)
  kClearStencil = 32,         // start each frame's stencil at zero (D3D9 keeps it: test only)
};
void SetSkip(unsigned mask);
// Leaves out draws of one phase::Category (render_phase.h) from the next frame on;
// -1 for none.
void SetSkipCategory(int category);
// Bit per phase::Category that had draws in the last traced frame.
uint32_t CategoriesInTrace();
// Current D3D render state as the WebGPU backend tracked it.
DWORD RenderState(D3DRENDERSTATETYPE state);
DWORD TextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type);

// Writes every draw of the next frame to andreasgpu_logs/<name>.
void RequestTrace(const wchar_t* name);

}  // namespace agpu::wgb
