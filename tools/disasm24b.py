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
secs = []
for i in range(ns):
    o = so + i*40
    vs = struct.unpack_from("<I", data, o+8)[0]; va = struct.unpack_from("<I", data, o+12)[0]
    rs = struct.unpack_from("<I", data, o+16)[0]; rp = struct.unpack_from("<I", data, o+20)[0]
    secs.append((va, vs, rp, rs))
def va2off(va):
    rva = va - ib
    for va_, vs, rp, rs in secs:
        if va_ <= rva < va_ + max(vs, rs): return rp + (rva - va_)
    return None
def read(va, n):
    o = va2off(va); return data[o:o+n]
print("double @0x4b1218:", struct.unpack('<d', read(0x4b1218, 8))[0])
def dis(va, n):
    off = va2off(va); md = Cs(CS_ARCH_X86, CS_MODE_32)
    print(f"=== 0x{va:x} ===")
    for ins in md.disasm(data[off:off+n], va):
        print(f"0x{ins.address:x}: {ins.mnemonic:7s} {ins.op_str}")
dis(0x424b40, 0x50)
dis(0x424cd0, 0x60)
