#!/usr/bin/env python3
"""Lift the 3D engine (0x46c000..0x4a7000) instruction by instruction with
pcrecomp's lift32 (MIT, tools/third_party/pcrecomp) into recomp/lifted/.

The engine is machine-translated code: it keeps its registers in memory
(0x4e949c..), allocates frames with `add esp, [mem]` and moves values with
push/pop across calls.  Ghidra's decompile of it is wrong in places, and
those errors make the game draw no 3D at all, so the whole engine is
translated exactly instead.

Interfaces with the decompiled C (32-bit host pointers everywhere):
  * absolute addresses in the image are rebased with VAI();
  * a code address loaded as a value becomes a host function: lifted_entry_X
    for engine code, the C function (or bridge_X when it takes stack
    arguments / returns a value) otherwise;
  * the x86 stack is a private guest stack (runtime/lifted_rt.c); the C
    wrappers in engine_entry.c push their arguments there and enter;
  * the decompiled versions are dropped through recomp/overrides.txt.
"""
import os, sys, re
import pefile, capstone
from capstone import x86

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'third_party', 'pcrecomp'))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
import lift32
from export_parse import load_export

ENGINE_LO, ENGINE_HI = 0x46c000, 0x4a7000
IMG_LO, IMG_HI = 0x400000, 0x8c1000

fs = load_export(os.path.join(ROOT, 'recomp', 'export', 'functions.txt'))
by = {f.addr: f for f in fs}
entries = set(f.addr for f in fs if not f.thunk and f.addr < 0x4a7e30 and not f.failed)
FUNCS = sorted(f.addr for f in fs if ENGINE_LO <= f.addr < ENGINE_HI and not f.thunk
               and not f.failed and 'bad instruction data' not in f.code)
LIFTED = set(FUNCS)
BRIDGES = {}

pe = pefile.PE(os.path.join(ROOT, 'game', 'DAYTONA USA Deluxe.exe'))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


class Block:
    def __init__(self, start):
        self.start = start
        self.end = start
        self.instructions = []
        self.successors = []
        self.is_exit = False


class Func:
    def __init__(self, addr, name):
        self.address = addr
        self.name = name
        self.blocks = {}
        self.jump_targets = set()


class Insn:
    """The instruction shape pcrecomp's lifter expects."""
    def __init__(self, i):
        self.address, self.size, self.mnemonic, self.op_str = i.address, i.size, i.mnemonic, i.op_str
        self.bytes = bytes(i.bytes)
        self.operands = list(i.operands)
        self._i = i

    def get_branch_target(self):
        if self.operands and self.operands[0].type == x86.X86_OP_IMM:
            return self.operands[0].imm & 0xFFFFFFFF
        return None

    def __getattr__(self, n):
        return getattr(self._i, n)


def insn_at(a):
    i = next(md.disasm(pe.get_data(a - 0x400000, 16), a), None)
    return Insn(i) if i is not None else None


def build(addr):
    """Recursive descent over one function; follows jumps (shared tails too)."""
    seen = {}
    leaders = {addr}
    work = [addr]
    while work:
        a = work.pop()
        while a not in seen:
            i = insn_at(a)
            if i is None:
                break
            seen[a] = i
            m = i.mnemonic
            if m in ('ret', 'retn'):
                break
            if m == 'jmp':
                if i.operands and i.operands[0].type == x86.X86_OP_IMM:
                    t = i.operands[0].imm & 0xffffffff
                    leaders.add(t)
                    work.append(t)
                break
            if m.startswith('j') or m.startswith('loop'):
                t = i.operands[0].imm & 0xffffffff
                leaders.add(t)
                work.append(t)
                leaders.add(a + i.size)
            a += i.size
    f = Func(addr, 'lifted_%08x' % addr)
    cur = None
    for a in sorted(seen):
        if cur is None or a in leaders or a != cur.end:
            cur = Block(a)
            f.blocks[a] = cur
        cur.instructions.append(seen[a])
        cur.end = a + seen[a].size
    return f


def nparams(f):
    m = re.search(r'\((.*)\)\s*$', f.sig)
    if not m or m.group(1).strip() in ('', 'void'):
        return 0
    return m.group(1).count(',') + 1


def returns_value(f):
    return not f.sig.startswith('void ')


def c_target(t):
    """How lifted code reaches C function t (no return address on the stack)."""
    f = by[t]
    if nparams(f) or returns_value(f):
        BRIDGES[t] = f
        return 'bridge_%08x' % t
    return f.name


BODIES = [(f.addr, f.end) for f in fs if ENGINE_LO <= f.addr < ENGINE_HI and not f.thunk]
PENDING = []


INSN_STARTS = set()   # every instruction reachable in the engine CFGs
INSN_COVER = set()    # every byte those instructions occupy


PROMOTE = set()       # immediates that are clearly jumped to / called


def scan_cfgs():
    # every function and every direct-call target reachable from them
    work, seen = list(FUNCS), set()
    while work:
        a = work.pop()
        if a in seen:
            continue
        seen.add(a)
        for b in build(a).blocks.values():
            ins = b.instructions
            for k, i in enumerate(ins):
                INSN_STARTS.add(i.address)
                INSN_COVER.update(range(i.address, i.address + i.size))
                if i.mnemonic == 'call' and i.operands and i.operands[0].type == x86.X86_OP_IMM:
                    t = i.operands[0].imm & 0xffffffff
                    if ENGINE_LO <= t < ENGINE_HI and not (0x471e28 <= t < 0x478270):
                        work.append(t)
                if i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x'):
                    v = int(i.op_str.split(', ')[1], 16)
                    for j in ins[k + 1:k + 9]:
                        if j.mnemonic in ('call', 'jmp') and j.op_str == 'eax':
                            PROMOTE.add(v)
                            break


def is_immediate(loc, v):
    """Is the relocated dword at loc the immediate/displacement of an
    instruction inside a known function (rather than a table entry)?"""
    for back in range(1, 12):
        i = insn_at(loc - back)
        if i is None or i.address + i.size <= loc:
            continue
        for op in i.operands:
            if op.type == x86.X86_OP_IMM and (op.imm & 0xffffffff) == v:
                return True
            if op.type == x86.X86_OP_MEM and (op.mem.disp & 0xffffffff) == v:
                return True
    return False


def in_engine_code(v):
    return v in INSN_STARTS


RELOC_LOCS = set(e.rva + 0x400000 for b in pe.DIRECTORY_ENTRY_BASERELOC for e in b.entries if e.type == 3)


def code_expr(v):
    if v in RELOC_LOCS:
        return None   # the start of a pointer table, not code
    if v in LIFTED:
        return '(uint32_t)(uintptr_t)lifted_entry_%08x' % v
    if v not in entries and in_engine_code(v) and v in PROMOTE:
        # a code label used as a value (stored, later called): lift it too
        LIFTED.add(v)
        PENDING.append(v)
        return '(uint32_t)(uintptr_t)lifted_entry_%08x' % v
    if v in entries:
        return '(uint32_t)(uintptr_t)%s' % c_target(v)
    return None


class EngineLifter(lift32.Lifter):
    """Image addresses in operands are rebased (VAI) or turned into code
    pointers only when the PE relocates those bytes: a number that merely
    falls in the image range (0x400000 = 1.0 in 16.16 fixed point) stays a
    number."""
    _cur = None

    def _lift_instruction(self, insn):
        self._cur = insn
        return super()._lift_instruction(insn)

    def _relocated(self, kind):
        i = self._cur
        if i is None:
            return True
        off, size = (i.imm_offset, i.imm_size) if kind == 'imm' else (i.disp_offset, i.disp_size)
        return bool(off) and size == 4 and (i.address + off) in RELOC_LOCS

    def _fmt_read(self, op):
        if op.type == x86.X86_OP_IMM:
            v = op.imm & 0xffffffff
            if IMG_LO <= v < IMG_HI and not self._relocated('imm'):
                return super()._fmt_read(op)          # a plain number
            c = code_expr(v)
            if c:
                return c
            if IMG_LO <= v < IMG_HI:
                return 'VAI(0x%x)' % v
        return super()._fmt_read(op)

    def _fmt_mem_addr(self, mem):
        d = mem.disp & 0xffffffff
        s = super()._fmt_mem_addr(mem)
        if IMG_LO <= d < IMG_HI and self._relocated('disp'):
            for h in ('0x%X' % d, '0x%x' % d):
                if h in s:
                    return s.replace(h, 'VAI(0x%x)' % d, 1)
        return s


def main():
    lf = EngineLifter(func_names={}, lifted=None)
    scan_cfgs()
    bodies = []
    work = list(FUNCS)
    # engine code addresses stored in data (jump tables, handler tables):
    # each becomes an entry so the relocated pointer calls lifted code
    import struct
    img = pe.get_memory_mapped_image()
    for blk in pe.DIRECTORY_ENTRY_BASERELOC:
        for e in blk.entries:
            if e.type != 3:
                continue
            v = struct.unpack_from('<I', img, e.rva)[0]
            loc = e.rva + 0x400000
            # a run of 3+ consecutive relocated dwords is a table (decoded at a
            # random offset a jump table can look like `test [esi], imm32`)
            in_run = any(all(loc + 4 * (k + j) in RELOC_LOCS for j in range(3)) for k in (-2, -1, 0))
            if loc in INSN_COVER or (not in_run and is_immediate(loc, v)):
                continue          # an instruction's immediate, not a table entry
            if v not in LIFTED and v not in entries and ENGINE_LO <= v < ENGINE_HI and v not in RELOC_LOCS and                     not (0x471e28 <= v < 0x478270):
                LIFTED.add(v)
                work.append(v)
    # (instruction text, C call inserted before it): port features inside the
    # engine (runtime/lifted_rt.c)
    PORT_HOOKS = [
        # scenery (lifted_0047b008): start block + block count for the drawing
        # loop only - the blocks marked visible before it also steer the AI
        # cars' simulation, so those stay as in the original
        ('RECOMP_CALL(lifted_0047bbe8); /* 0x0047B3F2', 'lifted_draw_distance();'),
        ('goto L_0047B444; /* 0x0047B3FC', 'lifted_scenery_done();'),
        # car level of detail (0x4852ba in the AI update 0x480eb3): 4/3/2 by
        # distance for cars on visible blocks - the port keeps 4 (full model)
        ('MEM32(VAI(0x4e94b8)) = eax; /* 0x00481853', 'if (lifted_car_full(eax)) eax = 4;'),
        ('MEM32(VAI(0x4e94b8)) = eax; /* 0x004819D7', 'if (lifted_car_full(eax)) eax = 4;'),
        # the rest of the track through the far pass again (its corner test
        # culls what is out of view): the infield / the other side of an oval
        ('lifted_draw_distance(); /* port hook */', 'RECOMP_REGS_OUT(); lifted_far_rest(); RECOMP_REGS_IN();'),
        # the second pass (0x47bfd4): object blocks from the camera block's list
        ('RECOMP_CALL(lifted_0047bfd4); /* 0x0047B3F7', 'lifted_pvs_extend();'),
        # vertex transform (0x470089): near vertices beyond a side limit drop
        # their whole polygon - widened with the widescreen view
        ('ecx = MEM32(VAI(0x4e94e4)); /* 0x004700C6', 'lifted_side_limit();'),
        # the far part of that range (0x47b46c splits it by polygon count and
        # hands the far blocks to 0x47bd70 through the second CPU context)
        ('RECOMP_CALL(lifted_0047b46c); /* 0x0047B2F6', 'lifted_mark_behind();'),
        ('eax = MEM32(VAI(0x4e94a4)); /* 0x0047B2FB', 'lifted_far_extend();'),
        # far block culling (0x47e22d): skipped when all 4 corners are off
        # screen, even on different sides - big ground blocks across the view
        # vanished; count only corners off the same side
        ('MEM32(VAI(0x4e94c0)) = 0; /* 0x0047E30C', 'lifted_corner_reset();'),
        ('/* bt MEM32(VAI(0x4e94e0)), ((0) & 31) */ /* 0x0047E3E8', 'lifted_corner_point();'),
        ('eax = MEM32(VAI(0x4e94c0)); /* 0x0047E436', 'lifted_corner_result();'),
    ]

    def localize_regs(body):
        """RECOMP_LOCAL_REGS (lifted.h): the eight registers live in locals
        (host registers) instead of globals that every guest memory store may
        alias.  Load them after RECOMP_ENTER and write them back on every exit;
        the call macros in lifted.h sync them around calls."""
        body = re.sub(r'(RECOMP_ENTER\(0x[0-9A-Fa-f]+u\);)', r'\1 RECOMP_REGS_LOCALS', body, count=1)
        body = re.sub(r'\breturn;', 'RECOMP_RETURN();', body)
        body = re.sub(r'_st\[([0-7])\]', r'ST(\1)', body)   # x87 ring (lifted.h)
        for at, call in PORT_HOOKS:           # port changes at fixed instructions
            body = body.replace('    ' + at, '    ' + call + ' /* port hook */\n    ' + at)
        i = body.rstrip().rfind('}')
        return body[:i] + '    RECOMP_REGS_OUT();\n' + body[i:]

    done = set()
    while work or PENDING:
        if PENDING:
            work.extend(PENDING)
            del PENDING[:]
        a = work.pop(0)
        if a in done:
            continue
        done.add(a)
        body = lf.lift_function(build(a))

        def call(m):
            t = int(m.group(1), 16)
            if t in LIFTED or (ENGINE_LO <= t < ENGINE_HI and t not in entries):
                if t not in LIFTED:
                    LIFTED.add(t)
                    work.append(t)
                return 'RECOMP_CALL(lifted_%08x)' % t
            if t in entries:
                return 'RECOMP_CCALL(%s)' % c_target(t)
            return 'RECOMP_ICALL(0x%08Xu)' % t
        body = re.sub(r'RECOMP_CALL\(sub_([0-9A-Fa-f]{8})\)', call, body)
        body = localize_regs(body)
        bodies.append('/* %08x (%s) */' % (a, by[a].name if a in by else 'entry inside another function'))
        bodies.append(body)
        bodies.append('')

    text = '\n'.join(bodies)
    # float / double accesses may be unaligned (they fault on ARM):
    # recomp_unaligned.h
    text = text.replace('(float*)', '(recomp_f32u*)').replace('(double*)', '(recomp_f64u*)')
    refs = sorted(set(re.findall(r'\b(FUN_[0-9a-f]{8})\b', text)))
    head = ['/* generated by tools/lift_engine.py from DAYTONA USA Deluxe.exe - do not edit',
            ' * x86 -> C lifting by pcrecomp lift32 (MIT, tools/third_party/pcrecomp). */',
            '#define LIFTED_BODY 1', '#include "lifted.h"', '']
    head += ['void lifted_%08x(void);' % a for a in sorted(LIFTED)]
    head += ['void lifted_entry_%08x(void);' % a for a in sorted(LIFTED)]
    head += ['void bridge_%08x(void);' % b for b in sorted(BRIDGES)]
    head += ['void %s(void);' % r for r in refs]
    os.makedirs(os.path.join(ROOT, 'recomp', 'lifted'), exist_ok=True)
    open(os.path.join(ROOT, 'recomp', 'lifted', 'engine_lifted.c'), 'w', newline='\n').write(
        '\n'.join(head) + '\n\n' + text)

    w = ['/* generated by tools/lift_engine.py - glue between decompiled C and lifted code */',
         '#include "lifted.h"', '']
    # C functions called from lifted code with stack arguments / a result:
    # arguments are read from the guest stack (RECOMP_CCALL/ICALL push no
    # return address), the result goes to eax or the x87 stack.
    for a, f in sorted(BRIDGES.items()):
        n = nparams(f)
        args = ', '.join('MEM32(g_esp + %d)' % (4 * k) for k in range(n))
        decl = ', '.join(['uint32_t'] * n) or 'void'
        if not returns_value(f):
            w.append('void %s(%s);' % (f.name, decl))
            w.append('void bridge_%08x(void) { %s(%s); }' % (a, f.name, args))
        elif re.match(r'(float10|double|float)\b', f.sig):
            w.append('double %s(%s);' % (f.name, decl))
            w.append('void bridge_%08x(void) { double r = %s(%s); g_fp_top = (g_fp_top - 1) & 7; g_st[g_fp_top] = r; }'
                     % (a, f.name, args))
        else:
            w.append('uint32_t %s(%s);' % (f.name, decl))
            w.append('void bridge_%08x(void) { g_eax = %s(%s); }' % (a, f.name, args))
    # entries: from C into lifted code (arguments pushed on the guest stack)
    for a in sorted(LIFTED):
        w.append('void lifted_%08x(void);' % a)
        w.append('void lifted_entry_%08x(void) { LIFTED_ENTER(lifted_%08x); }' % (a, a))
        if a not in by or by[a].thunk or a not in FUNCS:
            continue
        f = by[a]
        n = nparams(f)
        ret = 'uint32_t' if returns_value(f) else 'void'
        params = ', '.join('uint32_t p%d' % k for k in range(n)) or 'void'
        pushes = ''.join(' PUSH32(g_esp, p%d);' % k for k in reversed(range(n)))
        w.append('%s %s(%s) { if (!g_esp) lifted_stack_init();%s LIFTED_ENTER(lifted_%08x);%s%s }' % (
            ret, f.name, params, pushes, a, (' g_esp += %d;' % (4 * n)) if n else '',
            ' return g_eax;' if ret != 'void' else ''))
    # entry wrapper -> raw lifted body, for tail jumps (no return address).
    # Code pointers in the image's tables resolve to the C-callable FUN_
    # wrappers, so those map to the raw body as well.
    cwrap = [a for a in sorted(LIFTED) if a in by and not by[a].thunk and a in FUNCS]
    w.append('const struct lifted_pair lifted_pairs[] = {')
    for a in sorted(LIFTED):
        w.append('    { lifted_entry_%08x, lifted_%08x },' % (a, a))
    for a in cwrap:
        w.append('    { (lifted_fn)%s, lifted_%08x },' % (by[a].name, a))
    w.append('};')
    w.append('const int lifted_pairs_count = %d;' % (len(LIFTED) + len(cwrap)))
    # indirect calls from lifted code: a C-callable wrapper would take its
    # arguments from the host stack, so call the guest-stack version instead
    w.append('const struct lifted_pair lifted_icall_map[] = {')
    for a in cwrap:
        w.append('    { (lifted_fn)%s, lifted_entry_%08x },' % (by[a].name, a))
    for a, f in sorted(BRIDGES.items()):
        w.append('    { (lifted_fn)%s, bridge_%08x },' % (f.name, a))
    w.append('};')
    w.append('const int lifted_icall_map_count = %d;' % (len(cwrap) + len(BRIDGES)))
    # every lifted entry, for relocated code pointers in the image's data
    w.append('typedef struct { uint32_t va; void *fn; } FuncEntry;')
    w.append('const FuncEntry recomp_functable_lifted[] = {')
    for a in sorted(LIFTED):
        w.append('    { 0x%08x, (void *)lifted_entry_%08x },' % (a, a))
    w.append('};')
    w.append('const int recomp_functable_lifted_count = %d;' % len(LIFTED))
    open(os.path.join(ROOT, 'recomp', 'lifted', 'engine_entry.c'), 'w', newline='\n').write('\n'.join(w) + '\n')

    ov = os.path.join(ROOT, 'recomp', 'overrides.txt')
    lines = open(ov).read().split('\n') if os.path.exists(ov) else []
    have = set(l.split()[0] for l in lines if l.strip() and not l.startswith('#'))
    with open(ov, 'a') as o:
        for a in FUNCS:
            if '%08x' % a not in have:
                o.write('%08x  # lifted instruction by instruction (tools/lift_engine.py)\n' % a)
    print('lifted %d functions (%d engine entries, %d C bridges)' % (len(LIFTED), len(FUNCS), len(BRIDGES)))


if __name__ == '__main__':
    main()
