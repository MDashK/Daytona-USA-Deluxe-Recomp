"""make_patch.py - builds the patch data of the converter (DayConv).

The CD's GAME folder (Daytona USA Deluxe, original pressing: the exe md5 is
c1e334a15d9472219441ed5f2bb89fb1) becomes the folder the port runs from:

  DAYTONA USA Deluxe.exe  v1.20E Direct3D update (SEGA's DDX_D3D.ZIP) with
                          the Wraithverge No-CD change and the winmm import
                          renamed _INMM (the exact file the port was built and
                          tested with, md5 ee476877f27b8c77905033726b9704af)
  Resource.dll            v1.20E (with the "Cancel" spelling fix)
  DPCTRL.dll              v1.20E
  Resource/640X480X16/MODE/*.BMP   new in v1.20E (time of day pictures)

The three changed files are stored as deltas against the CD's files (they
need the user's CD); the nine pictures are new in the update and stored whole.

Usage (developer side):
  python make_patch.py <extracted CD root> <working game folder> <out patch.bin>

Patch format (all little endian, zlib-compressed as a whole):
  "DAYP" u32 version=1, u32 count, then per entry:
    str target (relative, '/' separators), str source ('' = whole file),
    16 bytes source md5 (zeros if none), 16 bytes target md5, u32 target size,
    u32 ops length, ops
  str = u16 length + UTF-8.  ops: 0x00 varint offset, varint length = copy
  from the source; 0x01 varint length, bytes = add.
"""
import hashlib, os, struct, sys, zlib

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n: out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)

def delta(old, new, k=8, minlen=12):
    index = {}
    for i in range(len(old) - k + 1):
        index.setdefault(old[i:i + k], []).append(i)
    ops = bytearray()
    lit = bytearray()
    def flush():
        if lit:
            ops.extend(b'\x01' + varint(len(lit)) + lit)
            lit.clear()
    i, prev = 0, -1          # prev: where the last copy ended in old (sequential guess)
    n = len(new)
    while i < n:
        best_len, best_off = 0, 0
        cands = []
        if 0 <= prev < len(old): cands.append(prev)
        cands += index.get(new[i:i + k], [])[-16:]
        for o in cands:
            l = 0
            while i + l < n and o + l < len(old) and new[i + l] == old[o + l]: l += 1
            if l > best_len: best_len, best_off = l, o
        if best_len >= minlen:
            flush()
            ops.extend(b'\x00' + varint(best_off) + varint(best_len))
            i += best_len
            prev = best_off + best_len
        else:
            lit.append(new[i]); i += 1
            if prev >= 0: prev += 1
    flush()
    return bytes(ops)

def apply(old, ops):          # check: the same decoder as the tool's
    out = bytearray(); p = 0
    def rv():
        nonlocal p
        v = s = 0
        while True:
            b = ops[p]; p += 1
            v |= (b & 0x7f) << s; s += 7
            if not b & 0x80: return v
    while p < len(ops):
        t = ops[p]; p += 1
        if t == 0:
            o = rv(); l = rv(); out += old[o:o + l]
        else:
            l = rv(); out += ops[p:p + l]; p += l
    return bytes(out)

def find_ci(root, rel):
    """case-insensitive path lookup"""
    cur = root
    for part in rel.split('/'):
        names = {n.lower(): n for n in os.listdir(cur)}
        if part.lower() not in names: return None
        cur = os.path.join(cur, names[part.lower()])
    return cur

ENTRIES = [   # target in the output folder, source on the CD ('' = stored whole)
    ('DAYTONA USA Deluxe.exe', 'GAME/DAYTONA USA Deluxe.exe'),
    ('Resource.dll', 'GAME/Resource.dll'),
    ('DPCTRL.dll', 'GAME/dpctrl.dll'),
] + [('Resource/640X480X16/MODE/%s.BMP' % n, '') for n in
     ('BASE', 'DAWN', 'DAYTIME', 'FLOODLIT', 'MIDNIGHT', 'MIDSUM', 'MIRRORED', 'NORMAL', 'TWILIGHT')]

def main():
    cd, game, out = sys.argv[1:4]
    blob = bytearray(b'DAYP' + struct.pack('<II', 1, len(ENTRIES)))
    def s(t):
        b = t.encode('utf-8'); return struct.pack('<H', len(b)) + b
    for tgt, src in ENTRIES:
        new = open(find_ci(game, tgt), 'rb').read()
        if src:
            old = open(find_ci(cd, src), 'rb').read()
            ops = delta(old, new)
            assert apply(old, ops) == new, tgt
            smd5 = hashlib.md5(old).digest()
        else:
            ops = b'\x01' + varint(len(new)) + new
            smd5 = bytes(16)
        blob += s(tgt) + s(src) + smd5 + hashlib.md5(new).digest() + struct.pack('<II', len(new), len(ops)) + ops
        print('%-40s %8d bytes -> ops %7d' % (tgt, len(new), len(ops)))
    z = zlib.compress(bytes(blob), 9)
    open(out, 'wb').write(z)
    print('patch: %d bytes (%d uncompressed)' % (len(z), len(blob)))

if __name__ == '__main__':
    main()
