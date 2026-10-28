#include "config.h"

#include <windows.h>

#include <string>

namespace agpu {

const Config& GetConfig() {
  static const Config config = [] {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetConfig), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring ini(path);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\andreasgpu.ini";
    Config c;
    wchar_t present[16] = {};
    GetPrivateProfileStringW(L"renderer", L"present", L"d3d9", present, 16, ini.c_str());
    c.presentWebGPU = _wcsicmp(present, L"webgpu") == 0;
    c.record = GetPrivateProfileIntW(L"renderer", L"record", 0, ini.c_str()) != 0;
    c.mirror = !c.presentWebGPU && GetPrivateProfileIntW(L"renderer", L"mirror", 0, ini.c_str()) != 0;
    return c;
  }();
  return config;
}

}  // namespace agpu
