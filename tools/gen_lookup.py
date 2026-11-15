import struct
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

# name, VA, size in bytes
tables = [
 ("DAT_004ae258",0x4ae258,0x100),
 ("DAT_004ae280",0x4ae280,0x100),
 ("DAT_004ae2b0",0x4ae2b0,0x100),
 ("DAT_004ae370",0x4ae370,0x100),
 ("DAT_004ae430",0x4ae430,0x2a),
 ("DAT_00492e2c",0x492e2c,0x40),
 ("DAT_004da3c6",0x4da3c6,0x100),
]

with open("src/lookup_tables.c","w") as f:
    f.write("/* lookup_tables.c - static lookup tables extracted verbatim from the\n")
    f.write(" * original DAYTONA USA Deluxe.exe (.rdata/.data). The colour/material\n")
    f.write(" * tables (DAT_004e95xx) and the geometry buffers (DAT_0063xxxx) are\n")
    f.write(" * zero in the binary and are populated at runtime from SCREEN.BIN.\n")
    f.write(" */\n\n")
    f.write('#include "app.h"\n\n')
    for name,va,sz in tables:
        off = va2off(va)
        b = data[off:off+sz]
        f.write(f"u8 {name}[{sz}] = {{\n")
        for i in range(0, sz, 16):
            row = ", ".join(str(x) for x in b[i:i+16])
            f.write("    " + row + ",\n")
        f.write("};\n\n")
print("wrote src/lookup_tables.c")
