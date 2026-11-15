import struct
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
base = va_to_off(0x49bf6c)
print(f"table at file off 0x{base:x}")
# dump first 64 bytes as shorts
for i in range(0, 64, 2):
    v = struct.unpack_from("<h", data, base+i)[0]
    if i % 16 == 0:
        print()
        print(f"byte {i:3d} (angle {i//4*16:5d}): ", end="")
    print(f"{v:6d} ", end="")
print()
# show as [sin,cos] pairs (4-byte stride)
print("\n[sin,cos] pairs (4-byte stride):")
for n in range(8):
    s = struct.unpack_from("<h", data, base + n*4)[0]
    c = struct.unpack_from("<h", data, base + n*4 + 2)[0]
    print(f"  n={n}: sin={s:6d} cos={c:6d}")
# check how far the table extends: find where values stop looking like sin/cos
# table should be 4096 entries = 16384 bytes. Check entry 4095 and 4096
print("\nlast entries:")
print("  entry 4095 (byte 16380):", struct.unpack_from("<hh", data, base + 4095*4))
print("  entry 4096 (byte 16384):", struct.unpack_from("<hh", data, base + 4096*4))
