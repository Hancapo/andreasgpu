// Checks that the 32-bit webgpu_dawn.dll works: instance -> adapter -> device on a
// window surface, a WGSL pipeline, and a few presented frames of a triangle.
#include <windows.h>
#include <dbghelp.h>
#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const char kShader[] = R"(
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {
  var p = array(vec2f(0.0, 0.6), vec2f(-0.6, -0.6), vec2f(0.6, -0.6));
  return vec4f(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0, 0.5, 0.1, 1.0); }
)";

bool g_errors = false;

LONG WINAPI OnCrash(EXCEPTION_POINTERS* e) {
  void* addr = e->ExceptionRecord->ExceptionAddress;
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCWSTR>(addr), &mod);
  wchar_t name[MAX_PATH] = L"?";
  if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
  printf("CRASH %08lX at %p = %ls+0x%X (access %p)\n", e->ExceptionRecord->ExceptionCode, addr, name,
         static_cast<unsigned>(static_cast<char*>(addr) - reinterpret_cast<char*>(mod)),
         reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]));

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  CONTEXT ctx = *e->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Eip;
  frame.AddrFrame.Offset = ctx.Ebp;
  frame.AddrStack.Offset = ctx.Esp;
  frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_I386, process, GetCurrentThread(), &frame, &ctx, nullptr,
                                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
       ++i) {
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD lineDisp = 0;
    bool hasSym = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym);
    bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line);
    printf("  #%d %08llX %s %s:%lu\n", i, frame.AddrPC.Offset, hasSym ? sym->Name : "?",
           hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

std::string Str(wgpu::StringView s) { return std::string(s.data ? s.data : "", s.data ? s.length : 0); }

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetUnhandledExceptionFilter(OnCrash);
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instanceDesc{};
  instanceDesc.requiredFeatureCount = 1;
  instanceDesc.requiredFeatures = &kTimedWaitAny;
  wgpu::Instance instance = wgpu::CreateInstance(&instanceDesc);
  if (!instance) return printf("CreateInstance failed\n"), 1;

  wgpu::RequestAdapterOptions adapterOptions{};
  adapterOptions.backendType = argc > 1 && strcmp(argv[1], "d3d11") == 0 ? wgpu::BackendType::D3D11
                                                                          : wgpu::BackendType::D3D12;
  adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;
  wgpu::Adapter adapter;
  instance.WaitAny(instance.RequestAdapter(&adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a,
                                               wgpu::StringView message) {
                                             if (status == wgpu::RequestAdapterStatus::Success)
                                               adapter = std::move(a);
                                             else
                                               printf("RequestAdapter: %s\n", Str(message).c_str());
                                           }),
                   UINT64_MAX);
  if (!adapter) return 1;
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  printf("adapter: %s (%s) backend=%d\n", Str(info.device).c_str(), Str(info.description).c_str(),
         static_cast<int>(info.backendType));

  wgpu::DeviceDescriptor deviceDesc{};
  deviceDesc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        printf("device lost %d: %s\n", static_cast<int>(reason), Str(message).c_str());
      });
  deviceDesc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        g_errors = true;
        printf("device error %d: %s\n", static_cast<int>(type), Str(message).c_str());
      });
  wgpu::Device device;
  instance.WaitAny(adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                         [&](wgpu::RequestDeviceStatus status, wgpu::Device d,
                                             wgpu::StringView message) {
                                           if (status == wgpu::RequestDeviceStatus::Success)
                                             device = std::move(d);
                                           else
                                             printf("RequestDevice: %s\n", Str(message).c_str());
                                         }),
                   UINT64_MAX);
  if (!device) return 1;
  printf("device ok\n");

  HWND wnd = CreateWindowW(L"STATIC", L"dawn smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
  wgpu::SurfaceSourceWindowsHWND hwndSource{};
  hwndSource.hinstance = GetModuleHandleW(nullptr);
  hwndSource.hwnd = wnd;
  wgpu::SurfaceDescriptor surfaceDesc{};
  surfaceDesc.nextInChain = &hwndSource;
  wgpu::Surface surface = instance.CreateSurface(&surfaceDesc);
  wgpu::SurfaceCapabilities caps;
  surface.GetCapabilities(adapter, &caps);
  RECT rc;
  GetClientRect(wnd, &rc);
  wgpu::SurfaceConfiguration config{};
  config.device = device;
  config.format = caps.formats[0];
  config.width = rc.right;
  config.height = rc.bottom;
  config.presentMode = wgpu::PresentMode::Fifo;
  surface.Configure(&config);
  printf("surface ok %ux%u format=%d\n", config.width, config.height, static_cast<int>(config.format));

  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = kShader;
  wgpu::ShaderModuleDescriptor moduleDesc{};
  moduleDesc.nextInChain = &wgsl;
  wgpu::ShaderModule module = device.CreateShaderModule(&moduleDesc);
  wgpu::ColorTargetState target{};
  target.format = config.format;
  wgpu::FragmentState fragment{};
  fragment.module = module;
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor pipelineDesc{};
  pipelineDesc.vertex.module = module;
  pipelineDesc.fragment = &fragment;
  wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipelineDesc);

  printf("pipeline ok\n");
  for (int frame = 0; frame < 60; ++frame) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    wgpu::SurfaceTexture surfaceTexture;
    surface.GetCurrentTexture(&surfaceTexture);
    wgpu::RenderPassColorAttachment color{};
    color.view = surfaceTexture.texture.CreateView();
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {0.1, 0.2, 0.3, 1.0};
    wgpu::RenderPassDescriptor pass{};
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::RenderPassEncoder rp = encoder.BeginRenderPass(&pass);
    rp.SetPipeline(pipeline);
    rp.Draw(3);
    rp.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    surface.Present();
    instance.ProcessEvents();
  }
  DestroyWindow(wnd);
  printf(g_errors ? "dawn smoke FAILED\n" : "dawn smoke ok\n");
  return g_errors ? 1 : 0;
}
