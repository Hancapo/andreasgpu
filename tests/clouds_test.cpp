// Offline check of the volumetric clouds (shim/src/webgpu/clouds.*): creates a
// headless Dawn device, compiles every cloud pipeline, renders a few fixed scenes
// (32 frames each, so the temporal accumulation settles) over a plain sky and writes
// them as PNGs. Any Dawn validation or WGSL error is printed and fails the run.
//   agpu_clouds_test [output folder]
#include <windows.h>
#include <wincodec.h>
#include <webgpu/webgpu_cpp.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "webgpu/atmosphere.h"
#include "webgpu/clouds.h"

using namespace agpu::wgb;

namespace {

bool g_errors = false;
std::string Str(wgpu::StringView s) { return std::string(s.data ? s.data : "", s.data ? s.length : 0); }

using Mat = float[16];  // D3D row-vector order, row-major

void Multiply(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float v = 0;
      for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = v;
    }
}

bool Invert(const float* m, float* out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1 : 0);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
  return true;
}

// Left-handed look-at with Z up, and a D3D perspective projection (depth 0..1).
void Camera(const float eye[3], float yawDeg, float pitchDeg, float aspect, float* viewProj, float* inv) {
  float yaw = yawDeg * 3.14159265f / 180.0f, pitch = pitchDeg * 3.14159265f / 180.0f;
  float z[3] = {std::cos(pitch) * std::sin(yaw), std::cos(pitch) * std::cos(yaw), std::sin(pitch)};
  float up[3] = {0, 0, 1};
  float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
  float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
  for (float& v : x) v /= xl;
  float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  auto dot = [&](const float* a) { return a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]; };
  float view[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x), -dot(y), -dot(z), 1};
  float zn = 0.5f, zf = 1500.0f, ys = 1.0f / std::tan(35.0f * 3.14159265f / 180.0f), xs = ys / aspect;
  float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  Multiply(view, proj, viewProj);
  Invert(viewProj, inv);
}

bool WritePng(const std::wstring& path, const std::vector<uint8_t>& bgra, UINT w, UINT h) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
            SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()))) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  return ok;
}

struct Scene {
  const char* name;
  float eye[3], yaw, pitch;
  float sun[3];
  int weather;
  float fog[3];
  float sky[3];
};

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::wstring outDir = L".";
  bool temporal = !(argc > 2 && strcmp(argv[2], "notemporal") == 0);
  if (argc > 1) {
    std::string a = argv[1];
    outDir.assign(a.begin(), a.end());
  }
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instanceDesc{};
  instanceDesc.requiredFeatureCount = 1;
  instanceDesc.requiredFeatures = &kTimedWaitAny;
  wgpu::Instance instance = wgpu::CreateInstance(&instanceDesc);
  wgpu::RequestAdapterOptions adapterOptions{};
  adapterOptions.backendType = wgpu::BackendType::D3D12;
  adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;
  wgpu::Adapter adapter;
  instance.WaitAny(instance.RequestAdapter(&adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView m) {
                                             if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
                                             else printf("RequestAdapter: %s\n", Str(m).c_str());
                                           }),
                   UINT64_MAX);
  if (!adapter) return 1;
  wgpu::DeviceDescriptor deviceDesc{};
  deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
    g_errors = true;
    printf("device error %d: %s\n", static_cast<int>(type), Str(message).c_str());
  });
  wgpu::Device device;
  instance.WaitAny(adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                         [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView m) {
                                           if (status == wgpu::RequestDeviceStatus::Success) device = std::move(d);
                                           else printf("RequestDevice: %s\n", Str(m).c_str());
                                         }),
                   UINT64_MAX);
  if (!device) return 1;
  wgpu::Queue queue = device.GetQueue();

  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  if (!atmosphere::Init(device, queue, wgpu::TextureFormat::BGRA8Unorm)) return printf("atmosphere Init failed\n"), 1;
  if (!clouds::Init(device, queue, wgpu::TextureFormat::BGRA8Unorm)) return printf("Init failed\n"), 1;
  instance.ProcessEvents();
  QueryPerformanceCounter(&t1);
  printf("init (pipelines + noise generation queued) %.0f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
  if (g_errors) return 1;

  // Noise check: slice z = 0 of the shape texture, channels R G B A side by side.
  {
    const uint32_t N = 128;
    wgpu::BufferDescriptor nb{};
    nb.size = N * N * 4;
    nb.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    wgpu::Buffer noiseBuf = device.CreateBuffer(&nb);
    wgpu::CommandEncoder enc = device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{};
    src.texture = clouds::ShapeNoise();
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = noiseBuf;
    dst.layout.bytesPerRow = N * 4;
    dst.layout.rowsPerImage = N;
    wgpu::Extent3D size{N, N, 1};
    enc.CopyTextureToBuffer(&src, &dst, &size);
    wgpu::CommandBuffer cb = enc.Finish();
    queue.Submit(1, &cb);
    bool ok = false;
    instance.WaitAny(noiseBuf.MapAsync(wgpu::MapMode::Read, 0, N * N * 4, wgpu::CallbackMode::WaitAnyOnly,
                                       [&](wgpu::MapAsyncStatus st, wgpu::StringView) { ok = st == wgpu::MapAsyncStatus::Success; }),
                     UINT64_MAX);
    if (ok) {
      const uint8_t* d = static_cast<const uint8_t*>(noiseBuf.GetConstMappedRange(0, N * N * 4));
      std::vector<uint8_t> img(N * 4 * N * 4);
      for (uint32_t y = 0; y < N; ++y)
        for (uint32_t c = 0; c < 4; ++c)
          for (uint32_t x = 0; x < N; ++x) {
            uint8_t v = d[(y * N + x) * 4 + c];
            uint8_t* o = &img[(y * N * 4 + c * N + x) * 4];
            o[0] = o[1] = o[2] = v, o[3] = 255;
          }
      noiseBuf.Unmap();
      WritePng(outDir + L"\\noise_shape_slice0.png", img, N * 4, N);
    }
  }

  const uint32_t W = 1280, H = 720;
  wgpu::TextureDescriptor cd{};
  cd.size = {W, H, 1};
  cd.format = wgpu::TextureFormat::BGRA8Unorm;
  cd.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture color = device.CreateTexture(&cd);
  cd.format = wgpu::TextureFormat::Depth24PlusStencil8;
  cd.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  wgpu::Texture depth = device.CreateTexture(&cd);
  wgpu::TextureViewDescriptor depthOnly{};
  depthOnly.aspect = wgpu::TextureAspect::DepthOnly;
  wgpu::TextureView colorView = color.CreateView(), depthView = depth.CreateView(),
                    depthSample = depth.CreateView(&depthOnly);
  wgpu::BufferDescriptor rb{};
  rb.size = W * H * 4;
  rb.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = device.CreateBuffer(&rb);

  const Scene scenes[] = {
      {"noon_cumulus", {0, 0, 40}, 0, 18, {0.25f, 0.35f, 0.90f}, 0, {0.75f, 0.82f, 0.90f}, {0.36f, 0.55f, 0.85f}},
      // Fog colour and sun as logged in game at 10:47 (extra sunny LA).
      {"noon_game_fog", {0, 0, 220}, 0, 18, {0.60f, -0.42f, 0.68f}, 0, {0.20f, 0.58f, 0.91f}, {0.25f, 0.62f, 0.93f}},
      {"noon_sun_overhead", {0, 0, 40}, 0, 18, {0.0f, 0.01f, 1.0f}, 0, {0.75f, 0.82f, 0.90f}, {0.36f, 0.55f, 0.85f}},
      {"noon_yaw90", {0, 0, 40}, 90, 18, {0.25f, 0.35f, 0.90f}, 0, {0.75f, 0.82f, 0.90f}, {0.36f, 0.55f, 0.85f}},
      {"sunset_towards_sun", {0, 0, 40}, 270, 8, {-0.98f, 0.10f, 0.08f}, 5, {0.85f, 0.55f, 0.40f}, {0.55f, 0.45f, 0.55f}},
      {"cloudy_sf", {0, 0, 40}, 30, 15, {0.30f, 0.30f, 0.90f}, 7, {0.70f, 0.72f, 0.75f}, {0.55f, 0.60f, 0.68f}},
      {"looking_up", {0, 0, 40}, 60, 55, {0.40f, -0.20f, 0.89f}, 1, {0.75f, 0.82f, 0.90f}, {0.30f, 0.50f, 0.85f}},
      {"above_clouds", {0, 0, 5200}, 0, -25, {0.30f, 0.40f, 0.86f}, 4, {0.75f, 0.82f, 0.90f}, {0.36f, 0.55f, 0.85f}},
  };
  for (const Scene& sc : scenes) {
    clouds::ResetHistory();
    double renderMs = 0;
    for (int frameIndex = 0; frameIndex < 32; ++frameIndex) {
      clouds::Frame f{};
      Camera(sc.eye, sc.yaw, sc.pitch, float(W) / H, f.viewProj, f.invViewProj);
      std::memcpy(f.camera, sc.eye, sizeof(sc.eye));
      std::memcpy(f.fogColor, sc.fog, sizeof(sc.fog));
      f.fogEnd = 800.0f;  // as in game (haze 8000 m)
      float l = std::sqrt(sc.sun[0] * sc.sun[0] + sc.sun[1] * sc.sun[1] + sc.sun[2] * sc.sun[2]);
      for (int i = 0; i < 3; ++i) f.game.sun[i] = sc.sun[i] / l;
      f.game.weather[0] = f.game.weather[1] = static_cast<float>(sc.weather);
      f.game.env[1] = 0.3f;
      f.game.time[1] = 1000.0f + frameIndex / 60.0f;

      wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
      // The "scene": a plain sky colour, depth cleared to 1 (sky everywhere).
      wgpu::RenderPassColorAttachment ca{};
      ca.view = colorView;
      ca.loadOp = wgpu::LoadOp::Clear;
      ca.storeOp = wgpu::StoreOp::Store;
      ca.clearValue = {sc.sky[0], sc.sky[1], sc.sky[2], 1.0};  // RGBA, whatever the format
      wgpu::RenderPassDepthStencilAttachment da{};
      da.view = depthView;
      da.depthLoadOp = wgpu::LoadOp::Clear;
      da.depthStoreOp = wgpu::StoreOp::Store;
      da.depthClearValue = 1.0f;
      da.stencilLoadOp = wgpu::LoadOp::Clear;
      da.stencilStoreOp = wgpu::StoreOp::Store;
      wgpu::RenderPassDescriptor pd{};
      pd.colorAttachmentCount = 1;
      pd.colorAttachments = &ca;
      pd.depthStencilAttachment = &da;
      encoder.BeginRenderPass(&pd).End();
      if (!temporal) clouds::ResetHistory();
      f.atmosphere = true;
      atmosphere::Frame af{};
      std::memcpy(af.invViewProj, f.invViewProj, sizeof(af.invViewProj));
      std::memcpy(af.camera, f.camera, sizeof(af.camera));
      af.game = f.game;
      atmosphere::Update(device, queue, encoder, af, W, H);
      atmosphere::Apply(device, encoder, colorView, depthSample);
      clouds::Render(device, queue, encoder, f, W, H, depthSample);
      clouds::Composite(device, encoder, colorView, depthSample);
      if (frameIndex == 31) {
        wgpu::TexelCopyTextureInfo src{};
        src.texture = color;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = W * 4;
        dst.layout.rowsPerImage = H;
        wgpu::Extent3D size{W, H, 1};
        encoder.CopyTextureToBuffer(&src, &dst, &size);
      }
      wgpu::CommandBuffer commands = encoder.Finish();
      QueryPerformanceCounter(&t0);
      queue.Submit(1, &commands);
      bool done = false;
      instance.WaitAny(queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                                                 [&](wgpu::QueueWorkDoneStatus, wgpu::StringView) { done = true; }),
                       UINT64_MAX);
      QueryPerformanceCounter(&t1);
      if (frameIndex >= 8) renderMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
    }
    bool mapped = false;
    instance.WaitAny(readback.MapAsync(wgpu::MapMode::Read, 0, W * H * 4, wgpu::CallbackMode::WaitAnyOnly,
                                       [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                         mapped = status == wgpu::MapAsyncStatus::Success;
                                       }),
                     UINT64_MAX);
    if (!mapped) return printf("map failed\n"), 1;
    const uint8_t* data = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, W * H * 4));
    std::vector<uint8_t> pixels(data, data + W * H * 4);
    readback.Unmap();
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    std::wstring path = outDir + L"\\clouds_" + std::wstring(sc.name, sc.name + strlen(sc.name)) + L".png";
    printf("%s: %.2f ms per frame (GPU, 1280x720) -> %s\n", sc.name, renderMs / 24, WritePng(path, pixels, W, H) ? "ok" : "PNG failed");
  }
  clouds::Shutdown();
  atmosphere::Shutdown();
  printf(g_errors ? "ERRORS\n" : "clouds test ok\n");
  return g_errors ? 1 : 0;
}
