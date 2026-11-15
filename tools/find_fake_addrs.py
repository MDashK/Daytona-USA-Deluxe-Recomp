"""Find globals Ghidra invented from plain numbers (e.g. `&DAT_00540000`
for the integer 0x540000).

An operand that really is an address carries a base relocation in the PE; a
number that only looks like one does not.  Each function is disassembled;
a referenced global counts as a number when every operand holding a value
near it is unrelocated (and at least one such operand exists).

Writes recomp/fake_addrs.txt: "<func> <value>" lines used by gen.py."""
import os, re, struct, sys, bisect
import pefile, capstone
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from export_parse import load_export

pe = pefile.PE(os.path.join(ROOT, 'game', 'DAYTONA USA Deluxe.exe'))
BASE = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
RELOC = set(e.rva + BASE for b in pe.DIRECTORY_ENTRY_BASERELOC for e in b.entries if e.type == 3)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
TOL = 0x100

def operands(f):
    """(value, relocated) for every 32-bit immediate/displacement in f."""
    code = img[f.addr - BASE:f.end - BASE]
    out = []
    for i in md.disasm(code, f.addr):
        for op in i.operands:
            if op.type == capstone.x86.X86_OP_IMM and i.imm_offset and i.imm_size == 4:
                out.append((op.imm & 0xffffffff, i.address + i.imm_offset in RELOC))
            elif op.type == capstone.x86.X86_OP_MEM and i.disp_offset and i.disp_size == 4:
                out.append((op.mem.disp & 0xffffffff, i.address + i.disp_offset in RELOC))
    return out

def fakes(f):
    ops = operands(f)
    res = []
    for g in f.globals:
        if g.addr is None or not (BASE <= g.addr < BASE + 0x4c1000):
            continue
        if not re.search(r'\b%s\b' % re.escape(g.name), f.code):
            continue
        near = [r for v, r in ops if g.addr - TOL <= v <= g.addr + TOL]
        exact = [r for v, r in ops if v == g.addr]
        if exact and not any(exact) and not any(near):
            res.append(g)
    # bare hex literals in the image range (gen.py makes them VAI addresses)
    for m in sorted(set(re.findall(r'\b0x([0-9a-fA-F]{6,8})\b', f.code))):
        v = int(m, 16)
        if not (BASE + 0x1000 <= v < BASE + 0x4c1000):
            continue
        exact = [r for x, r in ops if x == v]
        near = [r for x, r in ops if v - TOL <= x <= v + TOL]
        wide = [r for x, r in ops if v - 0x10000 <= x <= v + 0x10000]
        # a folded constant (no operand of its own) counts when no relocated
        # operand is anywhere near it
        if (exact and not any(exact) and not any(near)) or (not exact and not any(wide)):
            class L: pass
            g = L(); g.addr = v; g.name = '0x%x' % v
            res.append(g)
    return res

if __name__ == '__main__':
    fs = load_export(os.path.join(ROOT, 'recomp', 'export', 'functions.txt'))
    lines = []
    for f in fs:
        if f.thunk or f.end <= f.addr or 0x46c000 <= f.addr < 0x4a7000:
            continue
        for g in fakes(f):
            src = next((l.strip() for l in f.code.split('\n')
                        if re.search(r'\b%s\b' % re.escape(g.name), l) and not l.startswith('@@')), '')
            lines.append('%08x %08x  # %s: %s' % (f.addr, g.addr, g.name, src[:100]))
    open(os.path.join(ROOT, 'recomp', 'fake_addrs.txt'), 'w').write(
        '# globals Ghidra made from plain numbers (tools/find_fake_addrs.py)\n' + '\n'.join(lines) + '\n')
    print('\n'.join(lines))
    print(len(lines), 'numbers mistaken for addresses', file=sys.stderr)
