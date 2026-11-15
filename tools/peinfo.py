#!/usr/bin/env python3
"""Dump key PE metadata to identify compiler/linker and layout."""
import struct, sys

def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]

def main(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        print("not MZ"); return
    pe = u32(data, 0x3C)
    print("PE header offset: 0x%X" % pe)
    if data[pe:pe+4] != b"PE\x00\x00":
        print("bad PE sig"); return
    coff = pe + 4
    machine = u16(data, coff)
    nsec = u16(data, coff+2)
    ts = u32(data, coff+4)
    opt = coff + 20
    magic = u16(data, opt)
    print("Machine: 0x%04X  Sections: %d  Timestamp: 0x%08X  OptMagic: 0x%04X" % (machine, nsec, ts, magic))
    if magic == 0x10B:  # PE32
        maj_link = data[opt+2]; min_link = data[opt+3]
        entry = u32(data, opt+16)
        image_base = u32(data, opt+28)
        subsys = u16(data, opt+68)
        print("Linker version: %d.%d" % (maj_link, min_link))
        print("ImageBase: 0x%08X  EntryPoint(RVA): 0x%08X  Subsystem: %d" % (image_base, entry, subsys))
    # Rich header scan
    rich = data.find(b"Rich")
    if rich != -1 and rich > 0x40 and rich < pe:
        print("Rich header present at 0x%X" % rich)
        # decode compids
        key = u32(data, rich + 4)
        idx = rich - 4
        # danS marker before rich
        compids = []
        while idx > 0x40:
            val = u32(data, idx) ^ key
            if val == 0x536E6144:  # "DanS"
                break
            compids.append(val)
            idx -= 4
        compids = compids[::-1]
        # pairs of (product, count)
        if len(compids) % 2 == 0:
            for i in range(0, len(compids), 2):
                prod = compids[i]; cnt = compids[i+1]
                pid = prod >> 16
                build = prod & 0xFFFF
                print("  Rich: product 0x%04X build %d count %d" % (pid, build, cnt))
    # sections
    sec_off = opt + u16(data, coff+16)
    print("Sections:")
    for i in range(nsec):
        o = sec_off + i*40
        name = data[o:o+8].rstrip(b"\x00").decode("latin1")
        vsize = u32(data, o+8); va = u32(data, o+12)
        rsize = u32(data, o+16); rptr = u32(data, o+20)
        chars = u32(data, o+36)
        print("  %-8s VA=%08X VSz=%08X RSz=%08X RPtr=%08X Chars=%08X" % (name, va, vsize, rsize, rptr, chars))

if __name__ == "__main__":
    main(sys.argv[1])
