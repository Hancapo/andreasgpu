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

// After CreateDevice / Reset: D3D state is back to defaults.
void ResetDevice(UINT backBufferWidth, UINT backBufferHeight, BOOL autoDepthStencil);

// Resources (proxies from resources.h).
void TextureCreated(ResourceProxy* texture, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format);
void TextureWritten(ResourceProxy* texture, UINT level, const D3DLOCKED_RECT& locked, const RECT* rect);
void BufferCreated(ResourceProxy* buffer, UINT length, DWORD usage, D3DPOOL pool, bool index, D3DFORMAT indexFormat);
void BufferWritten(ResourceProxy* buffer, UINT offset, UINT size, const void* data);
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

// The game is about to play an FMV in a DirectShow child window of the game window:
// hide the WebGPU surface until the next presented frame.
void MovieStarting();

// Comparison harness: the next Present also reads the WebGPU back buffer back.
void RequestCapture();
bool TakeCapture(std::vector<uint8_t>* bgra, UINT* width, UINT* height);

}  // namespace agpu::wgb
