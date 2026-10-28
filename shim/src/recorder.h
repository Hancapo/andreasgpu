#pragma once
// Records what vanilla SA asks of D3D9 so the WebGPU backend can be sized from data:
//  - every distinct fixed-function/shader state combination used by a draw, with
//    draw counts and the gta_sa.exe call sites that issued it;
//  - vertex declarations, shader bytecode, texture/buffer/render-target formats,
//    and per-method call counts.
// Fed by DeviceProxy. The game drives D3D9 from one thread (RenderWare, no
// D3DCREATE_MULTITHREADED), so nothing here locks.
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <string>

namespace agpu::rec {

enum DrawApi : uint8_t { kDrawPrimitive, kDrawIndexedPrimitive, kDrawPrimitiveUP, kDrawIndexedPrimitiveUP };

// Device state starts at the D3D9 defaults after CreateDevice/Reset (the game
// creates a pure device, so it can't be queried).
void ResetState(BOOL autoDepthStencil);
// Per-draw recording and the periodic dumps only run with record=1 (andreasgpu.ini).
bool Enabled();
void SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
void SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value);
void SetTexture(DWORD stage, IDirect3DBaseTexture9* texture);
void SetFvf(DWORD fvf);
void SetVertexDeclaration(IDirect3DVertexDeclaration9* decl);
void SetVertexShader(IDirect3DVertexShader9* shader);
void SetPixelShader(IDirect3DPixelShader9* shader);
void SetStreamSource(UINT stream, bool bound);
void SetLight(DWORD index, const D3DLIGHT9* light);
void LightEnable(DWORD index, BOOL enable);
void SetTransform(D3DTRANSFORMSTATETYPE state);

// `sp` is the proxy method's return-address slot; used to find gta_sa.exe callers.
void Draw(DrawApi api, D3DPRIMITIVETYPE prim, UINT primCount, const uintptr_t* sp);
void OnPresent();

void VertexDeclarationCreated(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9* decl);
void VertexShaderCreated(const DWORD* code, IDirect3DVertexShader9* shader, uintptr_t caller);
void PixelShaderCreated(const DWORD* code, IDirect3DPixelShader9* shader, uintptr_t caller);

// Counts one occurrence of a printf-formatted resource/target description.
void Resource(const char* fmt, ...);
std::string FormatName(DWORD format);
std::string SurfaceDesc(IDirect3DSurface9* surface);
std::string FilterName(DWORD filter);
void LogCaps(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type);

// Rewrites draw_states.txt and summary.txt in the log folder.
void Dump(const char* reason);

}  // namespace agpu::rec
