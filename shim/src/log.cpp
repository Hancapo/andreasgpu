#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace agpu {
namespace {

std::wstring g_dir;
FILE* g_log = nullptr;

std::wstring ModuleFolder() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ModuleFolder), &self);
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring s(path);
  return s.substr(0, s.find_last_of(L"\\/"));
}

}  // namespace

void LogInit() {
  if (g_log) return;
  g_dir = ModuleFolder() + L"\\andreasgpu_logs";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  g_log = _wfopen(LogPath(L"andreasgpu.log").c_str(), L"w");
}

std::wstring LogPath(const wchar_t* fileName) { return g_dir + L"\\" + fileName; }

std::wstring ModuleRelativePath(const wchar_t* relative) { return ModuleFolder() + L"\\" + relative; }

void Logf(const char* fmt, ...) {
  if (!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
}

void LogFlush() {
  if (g_log) fflush(g_log);
}

void WriteLogFile(const wchar_t* fileName, const void* data, size_t size) {
  FILE* f = _wfopen(LogPath(fileName).c_str(), L"wb");
  if (!f) return;
  fwrite(data, 1, size, f);
  fclose(f);
}

}  // namespace agpu
