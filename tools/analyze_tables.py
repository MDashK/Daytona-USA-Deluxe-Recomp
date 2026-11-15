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
    name=data[o:o+8].rstrip(b'\x00').decode('latin1')
    vs=struct.unpack_from('<I',data,o+8)[0]; va=struct.unpack_from('<I',data,o+12)[0]
    rs=struct.unpack_from('<I',data,o+16)[0]; rp=struct.unpack_from('<I',data,o+20)[0]
    secs.append((name,va,vs,rp,rs))
def info(va):
    rva=va-ib
    for name,va_,vs,rp,rs in secs:
        if va_<=rva<va_+vs:
            init = rva < va_+rs  # within raw (initialized) part
            off = rp+(rva-va_)
            return name, init, off
    return None,None,None

tables = [
 ("DAT_004ae258",0x4ae258,0x100),
 ("DAT_004ae280",0x4ae280,0x100),
 ("DAT_004ae2b0",0x4ae2b0,0x100),
 ("DAT_004ae370",0x4ae370,0x100),
 ("DAT_004ae430",0x4ae430,0x2a),
 ("DAT_00492e2c",0x492e2c,0x40),
 ("DAT_004da3c6",0x4da3c6,0x100),
 ("DAT_004e95b0",0x4e95b0,0x40),
 ("DAT_004e95c4",0x4e95c4,0x20),
 ("DAT_004e95ce",0x4e95ce,0x20),
 ("DAT_004e95d2",0x4e95d2,0x20),
 ("DAT_004e95de",0x4e95de,0x40),
 ("DAT_004e95e0",0x4e95e0,0x40),
 ("DAT_004e95e2",0x4e95e2,0x40),
 ("DAT_004e95e4",0x4e95e4,0x40),
 ("DAT_004e95e6",0x4e95e6,0x40),
 ("DAT_004e95e8",0x4e95e8,0x40),
 ("DAT_004e95ea",0x4e95ea,0x40),
 ("DAT_004e95ec",0x4e95ec,0x40),
 ("DAT_004e95ee",0x4e95ee,0x40),
 ("DAT_004e95f0",0x4e95f0,0x40),
 ("DAT_004e962e",0x4e962e,0x10),
 ("DAT_004e963a",0x4e963a,0x40),
 ("DAT_004e9646",0x4e9646,0x10),
 ("DAT_004e9a36",0x4e9a36,0x20),
 ("DAT_004e9724",0x4e9724,0x20),
 ("DAT_004e759c",0x4e759c,0x20),
 ("DAT_004e75a8",0x4e75a8,0x20),
 ("DAT_004e75ac",0x4e75ac,0x20),
 ("DAT_004e7804",0x4e7804,0x20),
 ("DAT_004e7814",0x4e7814,0x20),
 ("DAT_004e7838",0x4e7838,0x20),
 ("DAT_004e7864",0x4e7864,0x20),
 ("DAT_00614812",0x614812,0x40),
 ("DAT_00637400",0x637400,0x40),
 ("DAT_0063b400",0x63b400,0x40),
 ("DAT_00640000",0x640000,0x40),
]
for name,va,sz in tables:
    sec, init, off = info(va)
    if sec is None:
        print(f"{name}: 0x{va:x} -> NOT IN ANY SECTION")
        continue
    b = data[off:off+min(sz,64)]
    nonzero = any(x!=0 for x in b)
    hexdump = b[:16].hex(' ')
    print(f"{name}: 0x{va:x} [{sec}] init={init} nonzero={nonzero} first16={hexdump}")
