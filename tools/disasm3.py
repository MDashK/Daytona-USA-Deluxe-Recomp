import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

exe = "game/DAYTONA USA Deluxe.exe"
data = open(exe, "rb").read()
pe_off = struct.unpack_from("<I", data, 0x3c)[0]
num_sections = struct.unpack_from("<H", data, pe_off+6)[0]
opt_size = struct.unpack_from("<H", data, pe_off+20)[0]
opt_off = pe_off + 24
image_base = struct.unpack_from("<I", data, opt_off+28)[0]
sec_off = opt_off + opt_size
sections = []
for i in range(num_sections):
    o = sec_off + i*40
    name = data[o:o+8].rstrip(b'\x00').decode('latin1')
    vsize = struct.unpack_from("<I", data, o+8)[0]
    vaddr = struct.unpack_from("<I", data, o+12)[0]
    rawsize = struct.unpack_from("<I", data, o+16)[0]
    rawptr = struct.unpack_from("<I", data, o+20)[0]
    sections.append((name, vaddr, vsize, rawptr, rawsize))
def va_to_off(va):
    rva = va - image_base
    for name, vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None
def disasm(va, length):
    off = va_to_off(va)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    code = data[off:off+length]
    print(f"\n=== 0x{va:x} ===")
    for ins in md.disasm(code, va):
        print(f"0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}")
disasm(0x00450187, 0x180)
disasm(0x0048935b, 0x90)
