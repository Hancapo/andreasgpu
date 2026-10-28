import sys, struct, re
data = open(sys.argv[1],'rb').read()
# PE section mapping for VA
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe+6)[0]
optsz = struct.unpack_from('<H', data, pe+20)[0]
base = struct.unpack_from('<I', data, pe+24+28)[0]
secs=[]
for i in range(nsec):
    o = pe+24+optsz+i*40
    name=data[o:o+8].rstrip(b'\0').decode()
    vsz,va,rsz,rptr=struct.unpack_from('<IIII',data,o+8)
    secs.append((name,va,rsz,rptr))
def va_of(off):
    for n,va,rsz,rptr in secs:
        if rptr<=off<rptr+rsz: return base+va+off-rptr, n
    return None,None
res=[]
for m in re.finditer(rb'[\x00-\x04][\x01-\x03][\xfe\xff]\xff', data):
    off=m.start()
    if off%4: continue
    tok=struct.unpack_from('<I',data,off)[0]
    kind='vs' if (tok>>16)==0xfffe else 'ps'
    major=(tok>>8)&0xff; minor=tok&0xff
    if major not in (1,2,3) or minor>4: continue
    # find end token within 64KB, aligned
    end=None
    for p in range(off+4, min(off+65536,len(data)), 4):
        if struct.unpack_from('<I',data,p)[0]==0x0000FFFF: end=p+4;break
    if not end: continue
    va,sec=va_of(off)
    # first instruction opcode sanity
    nxt=struct.unpack_from('<I',data,off+4)[0]
    res.append((off,va,sec,f'{kind}_{major}_{minor}',end-off,hex(nxt)))
for r in res: print(hex(r[0]), hex(r[1]) if r[1] else None, r[2], r[3], r[4], r[5])
print(len(res), secs)
