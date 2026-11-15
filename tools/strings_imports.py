#!/usr/bin/env python3
"""Extract import table (DLL + function names) and printable ASCII strings."""
import struct, sys, re

def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]

def main(path):
    data = open(path, "rb").read()
    pe = u32(data, 0x3C)
    coff = pe + 4
    nsec = u16(data, coff + 2)
    opt = coff + 20
    magic = u16(data, opt)
    if magic != 0x10B:
        print("not PE32"); return
    sec_off = opt + u16(data, coff + 16)
    secs = []
    for i in range(nsec):
        o = sec_off + i*40
        name = data[o:o+8].rstrip(b"\x00").decode("latin1")
        vsize = u32(data, o+8); va = u32(data, o+12)
        rsize = u32(data, o+16); rptr = u32(data, o+20)
        secs.append((name, va, vsize, rptr, rsize))
    def rva_to_off(rva):
        for name, va, vsize, rptr, rsize in secs:
            if va <= rva < va + max(vsize, rsize):
                return rptr + (rva - va)
        return None
    # import table = data dir[1] at opt+104
    imp_rva = u32(data, opt + 104)
    imp_off = rva_to_off(imp_rva)
    if imp_off is None:
        print("no import table");
    else:
        print("=== IMPORTS ===")
        i = 0
        while True:
            d = imp_off + i*20
            oft = u32(data, d); name_rva = u32(data, d+12); ft = u32(data, d+16)
            if oft == 0 and name_rva == 0:
                break
            noff = rva_to_off(name_rva)
            dll = data[noff:noff+64].split(b"\x00")[0].decode("latin1") if noff else "?"
            print("[%s]" % dll)
            # original first thunk
            t = oft if oft else ft
            toff = rva_to_off(t)
            j = 0
            while True:
                th = u32(data, toff + j*4)
                if th == 0: break
                if th & 0x80000000:
                    print("   ord #%d" % (th & 0xFFFF))
                else:
                    hoff = rva_to_off(th)
                    # skip hint (2 bytes)
                    fn = data[hoff+2:hoff+128].split(b"\x00")[0].decode("latin1", "replace")
                    print("   %s" % fn)
                j += 1
            i += 1
    # ASCII strings
    print("\n=== ASCII STRINGS ===")
    text = data
    strings = re.findall(rb"[\x20-\x7e]{5,}", text)
    seen = set()
    for s in strings:
        s = s.decode("latin1")
        if s not in seen:
            seen.add(s)
            print(s)

if __name__ == "__main__":
    main(sys.argv[1])
