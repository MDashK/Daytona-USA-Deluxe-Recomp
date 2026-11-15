import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
exe = "game/DAYTONA USA Deluxe.exe"
data = open(exe, "rb").read()
pe_off = struct.unpack_from("<I", data, 0x3c)[0]
ns = struct.unpack_from("<H", data, pe_off+6)[0]
os_ = struct.unpack_from("<H", data, pe_off+20)[0]
oo = pe_off + 24
ib = struct.unpack_from("<I", data, oo+28)[0]
so = oo + os_
secs=[]
for i in range(ns):
    o=so+i*40
    vs=struct.unpack_from('<I',data,o+8)[0]; va=struct.unpack_from('<I',data,o+12)[0]
    rs=struct.unpack_from('<I',data,o+16)[0]; rp=struct.unpack_from('<I',data,o+20)[0]
    secs.append((va,vs,rp,rs))
def va2off(va):
    rva=va-ib
    for va_,vs,rp,rs in secs:
        if va_<=rva<va_+max(vs,rs): return rp+(rva-va_)
    return None
def read(va,n):
    o=va2off(va); return data[o:o+n]
def dis(va,n):
    off=va2off(va); md=Cs(CS_ARCH_X86, CS_MODE_32)
    print(f"=== 0x{va:x} ===")
    for ins in md.disasm(data[off:off+n], va):
        print(f"0x{ins.address:x}: {ins.mnemonic:7s} {ins.op_str}")
# jumptable at 0x488100 — read 8 pointers
print("jumptable @0x488100:")
for i in range(8):
    p = struct.unpack_from("<I", read(0x488100 + i*4, 4))[0]
    print(f"  [{i}] = 0x{p:x}")
# dump the 8 octant stubs (each ~10-20 bytes after the jumptable)
dis(0x488100, 0x40)
# dump the code around the jumptable targets (the octant adjust stubs)
# the jumptable entries point into code right after the table
dis(0x488120, 0x140)
