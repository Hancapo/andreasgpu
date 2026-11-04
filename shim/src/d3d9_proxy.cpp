// Hand-written IDirect3D9 / IDirect3DDevice9 proxy methods: borderless CreateDevice
// and Reset, plus the calls the recorder needs. Everything else forwards from
// gen/d3d9_proxy_forward.cpp.
//
// Why a proxy and not vtable patching: the D3D9 runtime gives each device a private
// heap copy of its vtable and rewrites the hot state-setter entries (SetRenderState,
// SetTexture, SetSamplerState, ...) while the game runs, silently dropping patches.
#include <intrin.h>

#include "compare.h"
#include "config.h"
#include "display.h"
#include "gen/d3d9_proxy.h"
#include "mouse.h"
#include "log.h"
#include "patch.h"
#include "recorder.h"
#include "webgpu/backend.h"
#include "webgpu/mirror.h"
#include "widescreen.h"

namespace agpu {
namespace {

#define SP() static_cast<const uintptr_t*>(_AddressOfReturnAddress())
#define COUNT(method) DeviceCallScope callScope(DeviceCall::method)

HWND g_window = nullptr;  // the game creates a single device

// Present mode (present=webgpu): the real D3D9 device renders into this hidden
// window and the game window belongs to the WebGPU surface.
HWND HelperWindow() {
  static HWND helper = [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AndreasGPUD3D9Helper";
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"AndreasGPU D3D9 helper", WS_POPUP, 0, 0, 64, 64,
                           nullptr, nullptr, wc.hInstance, nullptr);
  }();
  return helper;
}

// Parameters for the real device: borderless game window, and in present mode the
// helper window as the device window.
D3DPRESENT_PARAMETERS RealPresentParams(const D3DPRESENT_PARAMETERS& game) {
  D3DPRESENT_PARAMETERS used = BorderlessPresentParams(game, g_window);
  if (GetConfig().presentWebGPU) used.hDeviceWindow = HelperWindow();
  return used;
}

// Present mode with a running WebGPU backend: the real device only keeps state,
// resources and frame pacing; nothing is drawn on it.
bool WebGPUOwnsFrame() { return GetConfig().presentWebGPU && wgb::Active(); }

void CopyBack(D3DPRESENT_PARAMETERS* game, const D3DPRESENT_PARAMETERS& used) {
  HWND deviceWindow = game->hDeviceWindow;
  CopyBackPresentParams(game, used);
  game->hDeviceWindow = deviceWindow;
}

}  // namespace

// ---------------------------------------------------------------------------
// IDirect3D9

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
    AddRef();
    *ppvObj = this;
    return S_OK;
  }
  return real_->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE Direct3D9Proxy::AddRef() { return real_->AddRef(); }

ULONG STDMETHODCALLTYPE Direct3D9Proxy::Release() {
  ULONG refs = real_->Release();
  if (refs == 0) delete this;
  return refs;
}

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                                       DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pp,
                                                       IDirect3DDevice9** ppReturnedDeviceInterface) {
  g_window = pp->hDeviceWindow ? pp->hDeviceWindow : hFocusWindow;
  D3DPRESENT_PARAMETERS used = RealPresentParams(*pp);
  HWND focus = GetConfig().presentWebGPU ? used.hDeviceWindow : hFocusWindow;
  IDirect3DDevice9* real = nullptr;
  HRESULT hr = real_->CreateDevice(Adapter, DeviceType, focus, BehaviorFlags, &used, &real);
  Logf("[device] CreateDevice adapter=%u type=%u flags=%08lX %ux%u fmt=%s game-windowed=%d depth=%s "
       "interval=%08X present-flags=%08lX -> %08X\n",
       Adapter, DeviceType, BehaviorFlags, used.BackBufferWidth, used.BackBufferHeight,
       rec::FormatName(used.BackBufferFormat).c_str(), pp->Windowed,
       used.EnableAutoDepthStencil ? rec::FormatName(used.AutoDepthStencilFormat).c_str() : "none",
       used.PresentationInterval, used.Flags, hr);
  CopyBack(pp, used);
  rec::LogCaps(real_, Adapter, DeviceType);
  LogFlush();
  if (FAILED(hr)) {
    *ppReturnedDeviceInterface = nullptr;
    return hr;
  }
  rec::ResetState(pp->EnableAutoDepthStencil);
  OnBackBuffer(used.BackBufferWidth, used.BackBufferHeight);
  auto device = new DeviceProxy(real, this);
  SetDeviceProxy(device);
  *ppReturnedDeviceInterface = device;
  if (GetConfig().webgpu()) {
    bool vsync = used.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!mirror::Start(GetConfig().presentWebGPU ? g_window : nullptr, vsync))
      Logf("[webgpu] disabled: startup failed\n");
  }
  wgb::ResetDevice(used.BackBufferWidth, used.BackBufferHeight, used.EnableAutoDepthStencil,
                      used.AutoDepthStencilFormat);
  return hr;
}

// ---------------------------------------------------------------------------
// IDirect3DDevice9

HRESULT STDMETHODCALLTYPE DeviceProxy::QueryInterface(REFIID riid, void** ppvObj) {
  COUNT(QueryInterface);
  if (!ppvObj) return E_POINTER;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
    AddRef();
    *ppvObj = this;
    return S_OK;
  }
  return real_->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE DeviceProxy::AddRef() { return real_->AddRef(); }

ULONG STDMETHODCALLTYPE DeviceProxy::Release() {
  ULONG refs = real_->Release();
  if (refs == 0) delete this;
  return refs;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::GetDirect3D(IDirect3D9** ppD3D9) {
  COUNT(GetDirect3D);
  if (!ppD3D9) return D3DERR_INVALIDCALL;
  d3d_->AddRef();
  *ppD3D9 = d3d_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::Reset(D3DPRESENT_PARAMETERS* pp) {
  COUNT(Reset);
  rec::Dump("reset");
  D3DPRESENT_PARAMETERS used = RealPresentParams(*pp);
  HRESULT hr = real_->Reset(&used);
  Logf("[device] Reset %ux%u fmt=%s game-windowed=%d depth=%s -> %08X\n", used.BackBufferWidth,
       used.BackBufferHeight, rec::FormatName(used.BackBufferFormat).c_str(), pp->Windowed,
       used.EnableAutoDepthStencil ? rec::FormatName(used.AutoDepthStencilFormat).c_str() : "none", hr);
  CopyBack(pp, used);
  if (SUCCEEDED(hr)) {
    rec::ResetState(pp->EnableAutoDepthStencil);
    OnBackBuffer(used.BackBufferWidth, used.BackBufferHeight);
    wgb::ResetDevice(used.BackBufferWidth, used.BackBufferHeight, used.EnableAutoDepthStencil,
                      used.AutoDepthStencilFormat);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                               HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
  COUNT(Present);
  rec::OnPresent();
  InstallWidescreen2D();  // RW's engine table is final once frames are presented
  KeepCursorInWindow(g_window);
  static const bool supported = IsSupportedBuild();
  if (supported) UpdateMouse(g_window);
  compare::BeforePresent(real_, g_window);
  HRESULT hr = real_->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  mirror::OnPresent();
  compare::AfterPresent();
  return hr;
}

void STDMETHODCALLTYPE DeviceProxy::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) {
  COUNT(SetGammaRamp);
  if (pRamp && iSwapChain == 0) wgb::SetGammaRamp(pRamp);
  real_->SetGammaRamp(iSwapChain, Flags, pRamp);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                                     D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture,
                                                     HANDLE* pSharedHandle) {
  COUNT(CreateTexture);
  if (rec::Enabled()) rec::Resource("Texture2D %-9s usage=%08lX pool=%u mips=%s%s", rec::FormatName(Format).c_str(), Usage, Pool,
                Levels == 1 ? "1" : "n", (Width & (Width - 1)) || (Height & (Height - 1)) ? " npot" : "");
  HRESULT hr = real_->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
  if (SUCCEEDED(hr)) {
    *ppTexture = Wrap(*ppTexture);
    wgb::TextureCreated(FindProxy(*ppTexture), Width, Height, (*ppTexture)->GetLevelCount(), Usage, Format);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                                         D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture,
                                                         HANDLE* pSharedHandle) {
  COUNT(CreateCubeTexture);
  if (rec::Enabled()) rec::Resource("TextureCube %-9s usage=%08lX pool=%u edge=%u mips=%u", rec::FormatName(Format).c_str(), Usage,
                Pool, EdgeLength, Levels);
  HRESULT hr = real_->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
  if (SUCCEEDED(hr)) *ppCubeTexture = Wrap(*ppCubeTexture);
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                                          IDirect3DVertexBuffer9** ppVertexBuffer,
                                                          HANDLE* pSharedHandle) {
  COUNT(CreateVertexBuffer);
  if (rec::Enabled()) rec::Resource("VertexBuffer usage=%08lX fvf=%08lX pool=%u", Usage, FVF, Pool);
  HRESULT hr = real_->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
  if (SUCCEEDED(hr)) {
    *ppVertexBuffer = Wrap(*ppVertexBuffer);
    wgb::BufferCreated(FindProxy(*ppVertexBuffer), Length, Usage, Pool, false, D3DFMT_UNKNOWN);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                         IDirect3DIndexBuffer9** ppIndexBuffer,
                                                         HANDLE* pSharedHandle) {
  COUNT(CreateIndexBuffer);
  if (rec::Enabled()) rec::Resource("IndexBuffer %s usage=%08lX pool=%u", rec::FormatName(Format).c_str(), Usage, Pool);
  HRESULT hr = real_->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
  if (SUCCEEDED(hr)) {
    *ppIndexBuffer = Wrap(*ppIndexBuffer);
    wgb::BufferCreated(FindProxy(*ppIndexBuffer), Length, Usage, Pool, true, Format);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
                                                   IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect,
                                                   D3DTEXTUREFILTERTYPE Filter) {
  COUNT(StretchRect);
  if (rec::Enabled()) rec::Resource("StretchRect %s -> %s filter=%s", rec::SurfaceDesc(pSourceSurface).c_str(),
                rec::SurfaceDesc(pDestSurface).c_str(), rec::FilterName(Filter).c_str());
  wgb::StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect);
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->StretchRect(Unwrap(pSourceSurface), pSourceRect, Unwrap(pDestSurface), pDestRect, Filter);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) {
  COUNT(ColorFill);
  if (rec::Enabled()) rec::Resource("ColorFill %s", rec::SurfaceDesc(pSurface).c_str());
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->ColorFill(Unwrap(pSurface), pRect, color);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
  COUNT(SetRenderTarget);
  if (rec::Enabled()) rec::Resource("SetRenderTarget[%lu] %s", RenderTargetIndex, rec::SurfaceDesc(pRenderTarget).c_str());
  if (RenderTargetIndex == 0) OnSetRenderTarget0(pRenderTarget);
  HRESULT hr = real_->SetRenderTarget(RenderTargetIndex, Unwrap(pRenderTarget));
  if (SUCCEEDED(hr)) wgb::SetRenderTarget(RenderTargetIndex, pRenderTarget);
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
  COUNT(SetDepthStencilSurface);
  if (rec::Enabled()) rec::Resource("SetDepthStencilSurface %s", rec::SurfaceDesc(pNewZStencil).c_str());
  wgb::SetDepthStencilSurface(pNewZStencil);
  return real_->SetDepthStencilSurface(Unwrap(pNewZStencil));
}

HRESULT STDMETHODCALLTYPE DeviceProxy::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color,
                                             float Z, DWORD Stencil) {
  COUNT(Clear);
  if (rec::Enabled()) rec::Resource("Clear flags=%lX rects=%s", Flags, Count ? "yes" : "no");
  if ((Flags & D3DCLEAR_ZBUFFER) && compare::ClearStencil()) Flags |= D3DCLEAR_STENCIL, Stencil = 0;
  wgb::Clear(Flags, Color, Z, Stencil);
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->Clear(Count, pRects, Flags, Color, Z, Stencil);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  COUNT(SetTransform);
  rec::SetTransform(State);
  wgb::SetTransform(State, pMatrix);
  return real_->SetTransform(State, pMatrix);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetLight(DWORD Index, CONST D3DLIGHT9* arg1) {
  COUNT(SetLight);
  rec::SetLight(Index, arg1);
  wgb::SetLight(Index, arg1);
  return real_->SetLight(Index, arg1);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::LightEnable(DWORD Index, BOOL Enable) {
  COUNT(LightEnable);
  rec::LightEnable(Index, Enable);
  wgb::LightEnable(Index, Enable);
  return real_->LightEnable(Index, Enable);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  COUNT(SetRenderState);
  rec::SetRenderState(State, Value);
  wgb::SetRenderState(State, Value);
  return real_->SetRenderState(State, Value);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
  COUNT(SetTexture);
  rec::SetTexture(Stage, pTexture);
  wgb::SetTexture(Stage, FindProxy(pTexture));
  return real_->SetTexture(Stage, Unwrap(pTexture));
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  COUNT(SetTextureStageState);
  rec::SetTextureStageState(Stage, Type, Value);
  wgb::SetTextureStageState(Stage, Type, Value);
  return real_->SetTextureStageState(Stage, Type, Value);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  COUNT(SetSamplerState);
  rec::SetSamplerState(Sampler, Type, Value);
  wgb::SetSamplerState(Sampler, Type, Value);
  return real_->SetSamplerState(Sampler, Type, Value);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                                     UINT PrimitiveCount) {
  COUNT(DrawPrimitive);
  rec::Draw(rec::kDrawPrimitive, PrimitiveType, PrimitiveCount, SP());
  static bool inStencilView = false;
  if (!inStencilView && PrimitiveType == D3DPT_TRIANGLEFAN && compare::StencilView() &&
      wgb::RenderState(D3DRS_STENCILENABLE) && wgb::RenderState(D3DRS_STENCILFUNC) == D3DCMP_LESSEQUAL) {
    // The shadow darkening quad: draw it once per stencil band in an opaque colour.
    // Goes through this proxy so both renderers (and the game's state) see it.
    inStencilView = true;
    const D3DRENDERSTATETYPE rsSaved[] = {D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_ALPHABLENDENABLE,
                                          D3DRS_ALPHATESTENABLE, D3DRS_TEXTUREFACTOR};
    const D3DTEXTURESTAGESTATETYPE tssSaved[] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1};
    DWORD rsValues[std::size(rsSaved)], tssValues[std::size(tssSaved)];
    for (size_t i = 0; i < std::size(rsSaved); ++i) rsValues[i] = wgb::RenderState(rsSaved[i]);
    for (size_t i = 0; i < std::size(tssSaved); ++i) tssValues[i] = wgb::TextureStageState(0, tssSaved[i]);
    SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    // Test is "ref func stencil"; later bands overwrite earlier ones.
    struct Band {
      DWORD func, ref;
      D3DCOLOR color;
    };
    constexpr Band kBands[] = {
        {D3DCMP_EQUAL, 0, 0xFF000050},       // 0: dark blue
        {D3DCMP_EQUAL, 1, 0xFFFF0000},       // 1: red
        {D3DCMP_EQUAL, 2, 0xFF00FF00},       // 2: green
        {D3DCMP_EQUAL, 3, 0xFFFFFF00},       // 3: yellow
        {D3DCMP_LESS, 3, 0xFFFF00FF},        // 4+: magenta
        {D3DCMP_LESSEQUAL, 128, 0xFFFFFFFF}, // 128+ (wrapped below zero): white
        {D3DCMP_EQUAL, 254, 0xFFFF8000},     // -2: orange
        {D3DCMP_EQUAL, 255, 0xFF00FFFF},     // -1: cyan
    };
    for (const Band& b : kBands) {
      SetRenderState(D3DRS_STENCILFUNC, b.func);
      SetRenderState(D3DRS_STENCILREF, b.ref);
      SetRenderState(D3DRS_TEXTUREFACTOR, b.color);
      DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    for (size_t i = 0; i < std::size(rsSaved); ++i) SetRenderState(rsSaved[i], rsValues[i]);
    for (size_t i = 0; i < std::size(tssSaved); ++i) SetTextureStageState(0, tssSaved[i], tssValues[i]);
    inStencilView = false;
    return D3D_OK;
  }
  wgb::DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::DrawIndexedPrimitive(D3DPRIMITIVETYPE arg0, INT BaseVertexIndex,
                                                            UINT MinVertexIndex, UINT NumVertices, UINT startIndex,
                                                            UINT primCount) {
  COUNT(DrawIndexedPrimitive);
  rec::Draw(rec::kDrawIndexedPrimitive, arg0, primCount, SP());
  wgb::DrawIndexedPrimitive(arg0, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->DrawIndexedPrimitive(arg0, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                                       CONST void* pVertexStreamZeroData,
                                                       UINT VertexStreamZeroStride) {
  COUNT(DrawPrimitiveUP);
  rec::Draw(rec::kDrawPrimitiveUP, PrimitiveType, PrimitiveCount, SP());
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                                              UINT NumVertices, UINT PrimitiveCount,
                                                              CONST void* pIndexData, D3DFORMAT IndexDataFormat,
                                                              CONST void* pVertexStreamZeroData,
                                                              UINT VertexStreamZeroStride) {
  COUNT(DrawIndexedPrimitiveUP);
  rec::Draw(rec::kDrawIndexedPrimitiveUP, PrimitiveType, PrimitiveCount, SP());
  if (WebGPUOwnsFrame()) return D3D_OK;
  return real_->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
                                       IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements,
                                                               IDirect3DVertexDeclaration9** ppDecl) {
  COUNT(CreateVertexDeclaration);
  HRESULT hr = real_->CreateVertexDeclaration(pVertexElements, ppDecl);
  if (SUCCEEDED(hr) && pVertexElements) {
    rec::VertexDeclarationCreated(pVertexElements, *ppDecl);
    wgb::VertexDeclarationCreated(*ppDecl, pVertexElements);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
  COUNT(SetVertexDeclaration);
  rec::SetVertexDeclaration(pDecl);
  wgb::SetVertexDeclaration(pDecl);
  return real_->SetVertexDeclaration(pDecl);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetFVF(DWORD FVF) {
  COUNT(SetFVF);
  rec::SetFvf(FVF);
  wgb::SetFVF(FVF);
  return real_->SetFVF(FVF);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
  COUNT(CreateVertexShader);
  HRESULT hr = real_->CreateVertexShader(pFunction, ppShader);
  if (SUCCEEDED(hr) && pFunction) {
    rec::VertexShaderCreated(pFunction, *ppShader, *SP());
    wgb::ShaderCreated(*ppShader, pFunction, true);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetVertexShader(IDirect3DVertexShader9* pShader) {
  COUNT(SetVertexShader);
  rec::SetVertexShader(pShader);
  wgb::SetVertexShader(pShader);
  return real_->SetVertexShader(pShader);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
                                                       UINT OffsetInBytes, UINT Stride) {
  COUNT(SetStreamSource);
  rec::SetStreamSource(StreamNumber, pStreamData != nullptr);
  wgb::SetStreamSource(StreamNumber, FindProxy(pStreamData), OffsetInBytes, Stride);
  return real_->SetStreamSource(StreamNumber, Unwrap(pStreamData), OffsetInBytes, Stride);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
  COUNT(CreatePixelShader);
  HRESULT hr = real_->CreatePixelShader(pFunction, ppShader);
  if (SUCCEEDED(hr) && pFunction) {
    rec::PixelShaderCreated(pFunction, *ppShader, *SP());
    wgb::ShaderCreated(*ppShader, pFunction, false);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetPixelShader(IDirect3DPixelShader9* pShader) {
  COUNT(SetPixelShader);
  rec::SetPixelShader(pShader);
  wgb::SetPixelShader(pShader);
  return real_->SetPixelShader(pShader);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetMaterial(CONST D3DMATERIAL9* pMaterial) {
  COUNT(SetMaterial);
  wgb::SetMaterial(pMaterial);
  return real_->SetMaterial(pMaterial);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetViewport(CONST D3DVIEWPORT9* pViewport) {
  COUNT(SetViewport);
  wgb::SetViewport(pViewport);
  return real_->SetViewport(pViewport);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
  COUNT(SetIndices);
  wgb::SetIndices(FindProxy(pIndexData));
  return real_->SetIndices(Unwrap(pIndexData));
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData,
                                                                UINT Vector4fCount) {
  COUNT(SetVertexShaderConstantF);
  wgb::SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
  return real_->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE DeviceProxy::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData,
                                                               UINT Vector4fCount) {
  COUNT(SetPixelShaderConstantF);
  wgb::SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
  return real_->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

}  // namespace agpu
