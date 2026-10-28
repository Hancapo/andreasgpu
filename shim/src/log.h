#pragma once
#include <cstddef>
#include <string>

namespace agpu {

// Output goes to "<folder of this dll>\andreasgpu_logs\".
void LogInit();
void Logf(const char* fmt, ...);
void LogFlush();
std::wstring LogPath(const wchar_t* fileName);
// Writes (overwrites) a file in the log folder.
void WriteLogFile(const wchar_t* fileName, const void* data, size_t size);
// "<folder of this dll>\<relative>".
std::wstring ModuleRelativePath(const wchar_t* relative);

}  // namespace agpu
