#!/usr/bin/env python3
"""Derive Ghidra prototype corrections from an export (recomp/export/functions.txt).

Writes recomp/fixups_auto.txt; ghidra/run_recomp_export.bat feeds
fixups_auto.txt + fixups_manual.txt back into RecompExport.java.

Heuristics:
  * varargs  - a function that takes the address of its last parameter
               (`&param_N`, the MSVC va_start idiom) becomes "N-1 params, ...".
  * ret      - a call to FUN_x followed by a read of `extraout_EAX` means FUN_x
               really returns a value in EAX: give it an undefined4 return.
  * remove   - functions created only from data pointers that lie inside the
               data blob embedded in .text (0x471e28..0x478270): they are model
               data, not code.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_parse import load_export

HERE = os.path.dirname(os.path.abspath(__file__))
DDSE = '--module' in sys.argv and sys.argv[sys.argv.index('--module') + 1] == 'ddse'
EXPORT_DIR = 'export_ddse' if DDSE else 'export'
CRT = 0x10010ec0 if DDSE else 0x4a7e30
PE_FILE = 'ddse.dll' if DDSE else 'DAYTONA USA Deluxe.exe'
AUTO = 'fixups_ddse_auto.txt' if DDSE else 'fixups_auto.txt'
BASE = 0x10000000 if DDSE else 0x400000
DATA_BLOBS = [(0x471e28, 0x478270)]
# addresses referenced like code labels that are really data (checked by hand)
NOT_CODE = {0x4a6dc0, 0x486ca0, 0x479ab0}   # 0x479ab0: a table of code pointers


def main():
    funcs = load_export(os.path.join(HERE, EXPORT_DIR, 'functions.txt'))
    old = set()
    oldidx = os.path.join(HERE, '..', 'decompiled_src', 'exported_functions.txt')
    if os.path.exists(oldidx):
        old = set(int(l.split('\t')[0], 16) for l in open(oldidx) if l.strip())
    out = []
    byaddr = {f.addr: f for f in funcs}
    byname = {f.name: f for f in funcs}
    for f in funcs:
        if f.addr >= CRT or f.thunk:
            continue
        # data blobs
        for lo, hi in ([] if DDSE else DATA_BLOBS):
            if lo <= f.addr < hi and (f.addr not in old or f.failed or 'bad instruction data' in f.code):
                out.append('remove %08x  # data inside .text' % f.addr)
                break
        if f.failed:
            continue
        nparams = len([l for l in f.locals if l.kind == 'P'])
        m = re.findall(r'&param_(\d+)', f.code)
        if m:
            k = max(int(x) for x in m)
            if k == nparams:
                out.append('varargs %08x %d  # &param_%d va_start idiom' % (f.addr, k - 1, k))
            else:
                out.append('# check %08x: &param_%d but %d params' % (f.addr, k, nparams))
        # va_start through a stack variable past the declared parameters
        if not m and not f.sig.rstrip().endswith('...)'):
            offs = [int(x, 16) for x in re.findall(r'&[a-z]{0,3}Stack([0-9a-f]{8})\b', f.code)]
            offs = [o for o in offs if o < 0x1000]
            if offs and re.search(r'vsprintf|wvsprintf', f.code):
                keep = (min(offs) - 4) // 4
                out.append('varargs %08x %d  # &xStack%08x va_start idiom' % (f.addr, keep, min(offs)))
        # extraout_EAX after a direct call
        lines = f.code.splitlines()
        for i, ln in enumerate(lines):
            if 'extraout_EAX' not in ln or re.match(r'\s*\w+\s+extraout_EAX', ln):
                continue
            for j in range(i - 1 if not re.search(r'FUN_[0-9a-f]{8}\(', ln.split('extraout_EAX')[0]) else i, -1, -1):
                cs = re.findall(r'\b(FUN_[0-9a-f]{8})\(', lines[j])
                if cs:
                    callee = byname.get(cs[-1])
                    if callee and not callee.thunk:
                        out.append('ret %08x undefined4  # extraout_EAX in %s' % (callee.addr, f.name))
                    break
    # code labels used as callbacks / jump-table targets -> functions
    import struct
    pe_path = os.path.join(HERE, '..', 'game', PE_FILE)
    try:
        import pefile
        pe = pefile.PE(pe_path)
        relocs = set()
        for b in pe.DIRECTORY_ENTRY_BASERELOC:
            for e in b.entries:
                if e.type == 3:
                    relocs.add(e.rva + BASE)
        text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
        tlo = BASE + text.VirtualAddress
        thi = tlo + text.Misc_VirtualSize
        def rd(a):
            return struct.unpack('<I', pe.get_data(a - BASE, 4))[0]
    except Exception as e:
        print('pefile unavailable:', e)
        relocs = None
    if relocs is not None:
        cand = set()
        dataidx = set()
        for f in funcs:
            for m in re.finditer(r'&LAB_([0-9a-f]{8})\s*\+', f.code):
                dataidx.add(int(m.group(1), 16))
        for f in funcs:
            if f.thunk or f.failed or f.addr >= CRT:
                continue
            for m in re.finditer(r'(=|\(|,)\s*(?:\([^()]*\))?\s*&LAB_([0-9a-f]{8})\s*(;|\)|,)', f.code):
                cand.add(int(m.group(2), 16))
            for m in re.finditer(r'PTR_LAB_([0-9a-f]{8})', f.code):
                a = int(m.group(1), 16)
                while a in relocs and tlo <= rd(a) < thi:
                    cand.add(rd(a))
                    a += 4
        for a in sorted(cand):
            if a in byaddr or not (tlo <= a < CRT):
                continue
            if a in dataidx or a in NOT_CODE:
                continue
            out.append('func %08x  # code label used as a pointer' % a)
    prev = os.path.join(HERE, AUTO)
    if os.path.exists(prev):
        for l in open(prev):
            if l.startswith(('ret ', 'varargs ', 'func ')):
                if l.startswith('func ') and int(l.split()[1], 16) in NOT_CODE:
                    continue
                out.append(l.rstrip())
    extra = os.path.join(HERE, 'uninit_calls.txt')
    if os.path.exists(extra) and not DDSE:
        out += [l.rstrip() for l in open(extra) if l.startswith('ret ')]
    seen = set()
    res = []
    for l in out:
        key = l.split('#')[0].strip()
        if key in seen:
            continue
        seen.add(key)
        res.append(l)
    with open(os.path.join(HERE, AUTO), 'w') as w:
        w.write('# generated by analyze_fixups.py - do not edit\n')
        w.write('\n'.join(res) + '\n')
    print('%d fixups' % len(res))


if __name__ == '__main__':
    main()
