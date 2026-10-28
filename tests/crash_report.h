#pragma once
// Prints the faulting address and a symbolised stack on an unhandled exception
// (no debugger is installed on the dev machine). Link with dbghelp.
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>

inline LONG WINAPI ReportCrash(EXCEPTION_POINTERS* e) {
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
    printf("  #%d %08llX %s %s:%lu\n", i, frame.AddrPC.Offset, hasSym ? sym->Name : "?", hasLine ? line.FileName : "",
           hasLine ? line.LineNumber : 0);
  }
  fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;
}
