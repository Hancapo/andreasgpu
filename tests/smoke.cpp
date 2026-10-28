// Loads the built shim by path, creates a device on a hidden window, issues a few
// fixed-function and shader draws, then unloads it. Checks the shim end to end
// without the game; the gta_sa.exe patches must refuse to install here.
#include <windows.h>
#include <d3d9.h>

#include <cstdio>

#include "crash_report.h"

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetUnhandledExceptionFilter(ReportCrash);
  if (argc < 2) {
    fwprintf(stderr, L"usage: smoke <path to shim d3d9.dll>\n");
    return 2;
  }
  HMODULE shim = LoadLibraryW(argv[1]);
  if (!shim) return fwprintf(stderr, L"LoadLibrary failed %lu\n", GetLastError()), 1;
  auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(shim, "Direct3DCreate9"));
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) return fwprintf(stderr, L"Direct3DCreate9 failed\n"), 1;

  HWND wnd = CreateWindowW(L"STATIC", L"smoke", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr, nullptr,
                           nullptr, nullptr);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                 &pp, &dev);
  if (FAILED(hr)) return fwprintf(stderr, L"CreateDevice failed %08lX\n", hr), 1;

  IDirect3DTexture9* tex = nullptr;
  dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
  const D3DVERTEXELEMENT9 elems[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
                                     {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
                                     D3DDECL_END()};
  IDirect3DVertexDeclaration9* decl = nullptr;
  dev->CreateVertexDeclaration(elems, &decl);
  // ps_1_1: tex t0; mov r0, t0
  const DWORD ps11[] = {0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000001, 0x800F0000, 0xB0E40000, 0x0000FFFF};
  IDirect3DPixelShader9* ps = nullptr;
  dev->CreatePixelShader(ps11, &ps);

  struct V { float x, y, z; DWORD c; } tri[3] = {{0, 0, 0, 0xFFFF0000}, {1, 0, 0, 0xFF00FF00}, {0, 1, 0, 0xFF0000FF}};

  // Resource proxy paths: texture lock, surface levels and identity, vertex buffers,
  // render-target queries, GetDevice, release to zero.
  auto check = [](bool ok, const char* what) {
    if (!ok) {
      printf("FAILED: %s\n", what);
      ExitProcess(1);
    }
  };
  D3DLOCKED_RECT lr{};
  check(SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)) && lr.pBits, "texture LockRect");
  memset(lr.pBits, 0xFF, 64 * 4);
  check(SUCCEEDED(tex->UnlockRect(0)), "texture UnlockRect");
  IDirect3DSurface9* level0 = nullptr;
  IDirect3DSurface9* level0Again = nullptr;
  check(SUCCEEDED(tex->GetSurfaceLevel(0, &level0)) && SUCCEEDED(tex->GetSurfaceLevel(0, &level0Again)),
        "GetSurfaceLevel");
  check(level0 == level0Again, "surface level identity");
  IDirect3DTexture9* container = nullptr;
  check(SUCCEEDED(level0->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&container))) &&
            container == tex,
        "GetContainer returns the texture proxy");
  container->Release();
  level0Again->Release();
  level0->Release();
  IDirect3DDevice9* owner = nullptr;
  check(SUCCEEDED(tex->GetDevice(&owner)) && owner == dev, "GetDevice returns the device proxy");
  owner->Release();
  IDirect3DSurface9* backBuffer = nullptr;
  check(SUCCEEDED(dev->GetRenderTarget(0, &backBuffer)) && backBuffer, "GetRenderTarget");
  check(SUCCEEDED(dev->SetRenderTarget(0, backBuffer)), "SetRenderTarget with a proxy");
  backBuffer->Release();
  IDirect3DVertexBuffer9* vb = nullptr;
  check(SUCCEEDED(dev->CreateVertexBuffer(sizeof(tri), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb,
                                          nullptr)),
        "CreateVertexBuffer");
  void* vbData = nullptr;
  check(SUCCEEDED(vb->Lock(0, 0, &vbData, D3DLOCK_DISCARD)) && vbData, "vertex buffer Lock");
  memcpy(vbData, tri, sizeof(tri));
  check(SUCCEEDED(vb->Unlock()), "vertex buffer Unlock");
  IDirect3DTexture9* scratch = nullptr;
  check(SUCCEEDED(dev->CreateTexture(16, 16, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &scratch, nullptr)), "DXT1 texture");
  check(scratch->Release() == 0, "texture released to zero");

  for (int frame = 0; frame < 3; ++frame) {
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    dev->BeginScene();
    dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetVertexDeclaration(decl);
    dev->SetPixelShader(ps);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 1, tri, sizeof(V));
    dev->SetStreamSource(0, vb, 0, sizeof(V));
    dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    dev->SetPixelShader(nullptr);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->EndScene();
    dev->Present(nullptr, nullptr, nullptr, nullptr);
  }
  dev->SetStreamSource(0, nullptr, 0, 0);
  dev->SetTexture(0, nullptr);
  vb->Release();
  ps->Release();
  decl->Release();
  tex->Release();
  dev->Release();
  d3d->Release();
  DestroyWindow(wnd);
  FreeLibrary(shim);  // exercises the revert-on-unload path
  wprintf(L"smoke ok\n");
  return 0;
}
