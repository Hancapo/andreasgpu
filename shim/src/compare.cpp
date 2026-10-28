#include "compare.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "webgpu/backend.h"

namespace agpu::compare {
namespace {

struct Image {
  std::vector<uint8_t> bgra;  // tightly packed
  UINT width = 0, height = 0;
};

bool g_pending = false;
Image g_d3d9;
unsigned g_index = 0;
ULONGLONG g_lastAuto = 0;

// Copies the D3D9 back buffer into system memory (resolving MSAA first).
bool ReadBackBuffer(IDirect3DDevice9* device, Image* out) {
  IDirect3DSurface9* back = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
  D3DSURFACE_DESC desc;
  back->GetDesc(&desc);
  bool ok = false;
  IDirect3DSurface9* resolved = nullptr;
  IDirect3DSurface9* source = back;
  if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
    if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                             &resolved, nullptr)) &&
        SUCCEEDED(device->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)))
      source = resolved;
  }
  IDirect3DSurface9* sysmem = nullptr;
  if ((desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
      SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sysmem,
                                                    nullptr)) &&
      SUCCEEDED(device->GetRenderTargetData(source, sysmem))) {
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      out->width = desc.Width;
      out->height = desc.Height;
      out->bgra.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
      for (UINT y = 0; y < desc.Height; ++y)
        memcpy(out->bgra.data() + static_cast<size_t>(y) * desc.Width * 4,
               static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch, desc.Width * 4);
      sysmem->UnlockRect();
      ok = true;
    }
  }
  if (!ok) Logf("[compare] could not read the D3D9 back buffer (format %u, msaa %u)\n", desc.Format, desc.MultiSampleType);
  if (sysmem) sysmem->Release();
  if (resolved) resolved->Release();
  back->Release();
  return ok;
}

bool WritePng(const std::wstring& path, const Image& image) {
  // Fails harmlessly (RPC_E_CHANGED_MODE) if the game thread already set up COM.
  static const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  (void)com;
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            format == GUID_WICPixelFormat32bppBGRA &&
            SUCCEEDED(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(image.bgra.size()),
                                         const_cast<BYTE*>(image.bgra.data()))) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (!ok) Logf("[compare] could not write %ls\n", path.c_str());
  return ok;
}

void Compare(Image& d3d9, Image& webgpu) {
  if (d3d9.width != webgpu.width || d3d9.height != webgpu.height) {
    Logf("[compare] size mismatch: d3d9 %ux%u, webgpu %ux%u\n", d3d9.width, d3d9.height, webgpu.width, webgpu.height);
    return;
  }
  const UINT w = d3d9.width, h = d3d9.height;
  constexpr int kGrid = 4;
  double sum[3] = {}, squared = 0, tile[kGrid][kGrid] = {};
  size_t bad = 0;
  Image diff{std::vector<uint8_t>(d3d9.bgra.size()), w, h};
  for (UINT y = 0; y < h; ++y) {
    for (UINT x = 0; x < w; ++x) {
      size_t i = (static_cast<size_t>(y) * w + x) * 4;
      int worst = 0, total = 0;
      for (int c = 0; c < 3; ++c) {
        int e = std::abs(int(d3d9.bgra[i + c]) - int(webgpu.bgra[i + c]));
        sum[c] += e;
        squared += double(e) * e;
        worst = std::max(worst, e);
        total += e;
        diff.bgra[i + c] = static_cast<uint8_t>(std::min(255, e * 4));
      }
      diff.bgra[i + 3] = 255;
      d3d9.bgra[i + 3] = webgpu.bgra[i + 3] = 255;  // X8R8G8B8 / blended alpha: show opaque
      bad += worst > 16;
      tile[y * kGrid / h][x * kGrid / w] += total / 3.0;
    }
  }
  const double pixels = double(w) * h;
  double mse = squared / (pixels * 3);
  double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
  std::wstring dir = LogPath(L"compare");
  CreateDirectoryW(dir.c_str(), nullptr);
  wchar_t prefix[32];
  swprintf(prefix, 32, L"\\%03u", g_index);
  WritePng(dir + prefix + L"_d3d9.png", d3d9);
  WritePng(dir + prefix + L"_webgpu.png", webgpu);
  WritePng(dir + prefix + L"_diff.png", diff);
  Logf("[compare] %03u %ux%u: mean error R %.2f G %.2f B %.2f, %.2f%% pixels off by >16, PSNR %.1f dB\n", g_index, w, h,
       sum[2] / pixels, sum[1] / pixels, sum[0] / pixels, 100.0 * bad / pixels, psnr);
  const double tilePixels = pixels / (kGrid * kGrid);
  for (int ty = 0; ty < kGrid; ++ty)
    Logf("[compare]     tiles row %d: %6.2f %6.2f %6.2f %6.2f\n", ty, tile[ty][0] / tilePixels, tile[ty][1] / tilePixels,
         tile[ty][2] / tilePixels, tile[ty][3] / tilePixels);
  LogFlush();
  ++g_index;
}

}  // namespace

void BeforePresent(IDirect3DDevice9* real, HWND window) {
  const Config& config = GetConfig();
  if (!wgb::Active()) return;
  bool due = false;
  // F10 (edge-triggered) while the game has focus.
  static bool keyWasDown = false;
  bool keyDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0 && GetForegroundWindow() == window;
  due |= keyDown && !keyWasDown;
  keyWasDown = keyDown;
  ULONGLONG now = GetTickCount64();
  if (config.mirror && config.compareSeconds > 0) {
    if (!g_lastAuto) g_lastAuto = now;
    if (now - g_lastAuto >= config.compareSeconds * 1000ull) {
      due = true;
      g_lastAuto = now;
    }
  }
  if (!due) return;
  // Without mirror mode the D3D9 device draws nothing: save the WebGPU frame only.
  g_d3d9 = {};
  if (config.mirror && !ReadBackBuffer(real, &g_d3d9)) return;
  g_pending = true;
  wgb::RequestCapture();
  wchar_t trace[48];
  swprintf(trace, 48, L"compare\\%03u_draws.txt", g_index);
  wgb::RequestTrace(trace);
}

void AfterPresent() {
  if (!g_pending) return;
  g_pending = false;
  Image webgpu;
  if (!wgb::TakeCapture(&webgpu.bgra, &webgpu.width, &webgpu.height)) {
    Logf("[compare] no WebGPU frame was captured\n");
    return;
  }
  if (g_d3d9.bgra.empty()) {
    for (size_t i = 3; i < webgpu.bgra.size(); i += 4) webgpu.bgra[i] = 255;
    std::wstring dir = LogPath(L"compare");
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[32];
    swprintf(name, 32, L"\\%03u_webgpu.png", g_index);
    if (WritePng(dir + name, webgpu)) Logf("[compare] %03u: saved the WebGPU frame\n", g_index);
    ++g_index;
    return;
  }
  Compare(g_d3d9, webgpu);
  g_d3d9 = {};
}

}  // namespace agpu::compare
