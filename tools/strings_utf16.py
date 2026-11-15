#!/usr/bin/env python3
"""Extract UTF-16LE strings from the .rsrc section (resource strings/dialogs)."""
import struct, sys, re

def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]

def main(path):
    data = open(path, "rb").read()
    pe = u32(data, 0x3C)
    coff = pe + 4
    nsec = u16(data, coff + 2)
    opt = coff + 20
    sec_off = opt + u16(data, coff + 16)
    rsrc_rptr = rsrc_size = None
    for i in range(nsec):
        o = sec_off + i*40
        name = data[o:o+8].rstrip(b"\x00").decode("latin1")
        rptr = u32(data, o+20); rsize = u32(data, o+16)
        if name == ".rsrc":
            rsrc_rptr, rsrc_size = rptr, rsize
    if rsrc_rptr is None:
        print("no .rsrc"); return
    blob = data[rsrc_rptr:rsrc_rptr+rsrc_size]
    # find runs of UTF-16LE printable
    strings = re.findall(rb"(?:[\x20-\x7e\x09\x0a\x0d]\x00){3,}", blob)
    seen = set()
    out = []
    for s in strings:
        try:
            txt = s.decode("utf-16-le")
        except Exception:
            continue
        if txt not in seen:
            seen.add(txt)
            out.append(txt)
    for t in out:
        print(t)

if __name__ == "__main__":
    main(sys.argv[1])
