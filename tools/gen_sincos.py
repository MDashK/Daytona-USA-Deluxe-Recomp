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
N = 4096  # entries
vals = [struct.unpack_from("<h", data, base + i*2)[0] for i in range(N*2)]  # 8192 shorts

with open("src/sincos_table.c", "w") as f:
    f.write("/* sincos_table.c — Q14 sine/cosine lookup table, extracted verbatim\n")
    f.write(" * from the original DAYTONA USA Deluxe.exe at VA 0x0049bf6c.\n")
    f.write(" *\n")
    f.write(" * Layout: 4096 entries, each a [sin, cos] pair (s16, scaled by 16384),\n")
    f.write(" * i.e. entry n = (sin(2*pi*n/4096)*16384, cos(2*pi*n/4096)*16384).\n")
    f.write(" * The rotation FUN_00489222 indexes this table at BYTE offset\n")
    f.write(" * ((angle + 8) & 0xfff0) >> 2 (stride 4 bytes per entry).\n")
    f.write(" */\n\n")
    f.write("#include \"app.h\"\n\n")
    f.write("s16 DAT_0049bf6c[0x2000] = {\n")
    for i in range(0, len(vals), 8):
        row = ", ".join(str(v) for v in vals[i:i+8])
        f.write("    " + row + ",\n")
    f.write("};\n")

print("wrote src/sincos_table.c with", len(vals), "shorts (", len(vals)*2, "bytes )")
