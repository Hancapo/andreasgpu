"""Disassemble D3D9 shader bytecode with the system d3dcompiler_47.dll.

  python -I disasm_shaders.py <gta_sa.exe> <file offset hex>...   blobs inside an exe
  python -I disasm_shaders.py <shader.bin>...                      blobs dumped by the shim
"""
import ctypes
import struct
import sys

d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
D3DDisassemble = d3dc.D3DDisassemble
D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p,
                           ctypes.POINTER(ctypes.c_void_p)]


def blob_at(data, off):
    p = off + 4
    while struct.unpack_from('<I', data, p)[0] != 0xFFFF:
        p += 4
    return data[off:p + 4]


def disassemble(blob):
    out = ctypes.c_void_p()
    hr = D3DDisassemble(blob, len(blob), 0, None, ctypes.byref(out)) & 0xFFFFFFFF
    if hr:
        return f'D3DDisassemble failed hr={hr:#x}'
    vt = ctypes.cast(out, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    return ctypes.string_at(get_ptr(out), get_size(out)).decode(errors='replace')


args = sys.argv[1:]
if args and args[0].lower().endswith('.exe'):
    data = open(args[0], 'rb').read()
    for off in (int(x, 16) for x in args[1:]):
        blob = blob_at(data, off)
        print(f'==== file+{off:#x} size {len(blob)}')
        print(disassemble(blob))
else:
    for path in args:
        blob = open(path, 'rb').read()
        print(f'==== {path} size {len(blob)}')
        print(disassemble(blob))
