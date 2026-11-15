"""extract_icon.py <exe> <out.ico> - the first icon group of a PE file as an .ico"""
import struct, sys

def main():
    d = open(sys.argv[1], 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    rsrc_rva = struct.unpack_from('<I', d, opt + 96 + 2 * 8)[0]
    secs = []
    for i in range(nsec):
        o = opt + optsz + i * 40
        vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, max(vs, rs), ra))
    def off(rva):
        for va, sz, ra in secs:
            if va <= rva < va + sz: return ra + rva - va
        raise ValueError(hex(rva))
    base = off(rsrc_rva)
    def entries(dir_off):
        n1, n2 = struct.unpack_from('<HH', d, base + dir_off + 12)
        for i in range(n1 + n2):
            name, ptr = struct.unpack_from('<II', d, base + dir_off + 16 + i * 8)
            yield name, ptr
    def leaf(ptr):              # follow down to the first data entry
        while ptr & 0x80000000:
            ptr = next(entries(ptr & 0x7fffffff))[1]
        rva, size = struct.unpack_from('<II', d, base + ptr)
        return d[off(rva):off(rva) + size]
    types = dict(entries(0))
    icons = {}
    for name, ptr in entries(types[3] & 0x7fffffff):        # RT_ICON by id
        icons[name] = leaf(ptr)
    grp = leaf(next(entries(types[14] & 0x7fffffff))[1])     # RT_GROUP_ICON
    _, _, count = struct.unpack_from('<HHH', grp, 0)
    hdr = struct.pack('<HHH', 0, 1, count)
    dirs, blobs = b'', b''
    pos = 6 + 16 * count
    for i in range(count):
        w, h, cc, r, planes, bpp, size, iid = struct.unpack_from('<BBBBHHIH', grp, 6 + i * 14)
        img = icons[iid]
        dirs += struct.pack('<BBBBHHII', w, h, cc, r, planes, bpp, len(img), pos + len(blobs))
        blobs += img
    open(sys.argv[2], 'wb').write(hdr + dirs + blobs)
    print('%d images' % count)

if __name__ == '__main__':
    main()
