#!/usr/bin/env python3
"""Turn the Ghidra export (recomp/export/) into compilable C (recomp/gen/).

Output:
  gen/types.h        Ghidra's data types, cleaned up for a C compiler
  gen/protos.h       prototypes of every recompiled game function
  gen/w32_protos.h   prototypes of the imported Win32/DirectX APIs (w32_ prefix)
  gen/game_NN.c      the game functions, ~80 per translation unit
  gen/functable.c    original address -> host function (used by runtime/image.c)
  gen/report.txt     everything that needs a human look

Transformations applied to every function body:
  * globals (DAT_/PTR_/s_/_DAT_/LAB_...) become macros addressing g_image
  * stack locals whose address is taken, plus arrays/structs, are placed in a
    single frame buffer at their original stack offsets, so code that walks
    from one local into the next (Ghidra split one struct into several
    variables) still works
  * uninitialised register artefacts (extraout_/unaff_/in_) start as 0
  * address literals Ghidra left numeric (0x4b5a24) are rebased with VAI()
  * imports get a w32_ prefix, statically linked CRT functions a crt_ prefix
  * x._N_M_ partial accesses become PIECE macros
  * varargs functions get a real va_list
"""
import os, re, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from export_parse import load_export, EXTERNS

# one module per recompiled binary: the game exe and DDSE.dll (2D sprite engine)
MODULES = {
    'game': dict(export='export', out='gen', crt_start=0x4a7e30, img=(0x401000, 0x89ec30),
                 prefix='game_', table='recomp_functable', prelude=''),
    'ddse': dict(export='export_ddse', out='gen_ddse', crt_start=0x10010ec0, img=(0x10001000, 0x10168000),
                 prefix='ddse_', table='recomp_functable_ddse',
                 prelude='#define RECOMP_IMG g_image_ddse\n#define RECOMP_BASE 0x10000000u\n#define RECOMP_SIZE 0x00168000u\n'),
}
MODULE = 'game'
for _i, _a in enumerate(sys.argv):
    if _a == '--module':
        MODULE = sys.argv[_i + 1]
MOD = MODULES[MODULE]
EXPORT = os.path.join(HERE, MOD['export'])
OUT = os.path.join(HERE, MOD['out'])
CRT_START = MOD['crt_start'] or 0xffffffff   # MSVC 4.2 static CRT starts here
IMG_LO, IMG_HI = MOD['img']                  # .text .. end of .data/.bss
PER_FILE = 80

# functions that are replaced by hand-written code in runtime/ (by address)
OVERRIDES_FILE = os.path.join(HERE, 'overrides.txt')

report = collections.defaultdict(list)

# numbers Ghidra printed as addresses (&DAT_00540000 for 0x540000):
# "<function> <value>" from tools/find_fake_addrs.py + a manual list
FAKE_ADDRS = set()
for _fn in ('fake_addrs.txt', 'fake_addrs_manual.txt'):
    _p = os.path.join(HERE, _fn)
    if MODULE == 'game' and os.path.exists(_p):
        for _l in open(_p):
            _w = _l.split('#')[0].split()
            if len(_w) >= 2:
                FAKE_ADDRS.add((int(_w[0], 16), int(_w[1], 16)))

# Imports Ghidra has no signature for (DirectX, the game's own _INMM.dll
# wrapper around winmm, ...).  '%' is replaced by the w32_ name.
EXTERN_SIGS = {
    'w32_DirectDrawCreate': 'int %(void *lpGUID, void *lplpDD, void *pUnkOuter)',
    'w32_DirectDrawEnumerateA': 'int %(void *lpCallback, void *lpContext)',
    'w32_DirectInputCreateA': 'int %(void *hinst, uint dwVersion, void *ppDI, void *punkOuter)',
    'w32_DirectSoundCreate': 'int %(void *lpGuid, void *ppDS, void *pUnkOuter)',
    'w32_DirectPlayCreate': 'int %(void *lpGUID, void *lplpDP, void *pUnk)',
    'w32_DirectPlayLobbyCreateA': 'int %(void *lpGUID, void *lplpDPL, void *pUnk, void *lpData, uint dwSize)',
    'w32_acmMetrics': 'uint %(void *hao, uint uMetric, void *pMetric)',
    'w32_imeGetTime': 'uint %(void)',
    'w32_ciSendCommandA': 'uint %(uint IDDevice, uint uMsg, uint fdwCommand, uint dwParam)',
    'w32_ciGetErrorStringA': 'int %(uint fdwError, char *lpszErrorText, uint cchErrorText)',
    'w32_mioOpenA': 'void *%(char *szFilename, void *lpmmioinfo, uint dwOpenFlags)',
    'w32_mioClose': 'uint %(void *hmmio, uint wFlags)',
    'w32_mioRead': 'int %(void *hmmio, char *pch, int cch)',
    'w32_mioSeek': 'int %(void *hmmio, int lOffset, int iOrigin)',
    'w32_mioDescend': 'uint %(void *hmmio, void *lpck, void *lpckParent, uint wFlags)',
    'w32_mioAscend': 'uint %(void *hmmio, void *lpck, uint wFlags)',
    'w32_mioGetInfo': 'uint %(void *hmmio, void *lpmmioinfo, uint wFlags)',
    'w32_mioSetInfo': 'uint %(void *hmmio, void *lpmmioinfo, uint wFlags)',
    'w32_mioAdvance': 'uint %(void *hmmio, void *lpmmioinfo, uint wFlags)',
    'w32_oyGetNumDevs': 'uint %(void)',
    'w32_oyGetDevCapsA': 'uint %(uint uJoyID, void *pjc, uint cbjc)',
    'w32_oyGetPosEx': 'uint %(uint uJoyID, void *pji)',
    'w32_oySetCapture': 'uint %(void *hwnd, uint uJoyID, uint uPeriod, int fChanged)',
    'w32_oyReleaseCapture': 'uint %(uint uJoyID)',
}


def crt_name(n):
    base = n.replace('FID_conflict:', '')
    return 'crt_' + re.sub(r'\W', '_', base)


def w32_name(n):
    return 'w32_' + re.sub(r'\W', '_', n)


def sanitize(n):
    return re.sub(r'\W', '_', n)


CC_KEYWORDS = re.compile(r'\b(__cdecl|__stdcall|__fastcall|__thiscall|__vectorcall|__noreturn|__regparm\d?)\b\s*')


def split_def(code, name):
    """Return (prefix_comments, definition_header, body) of Ghidra's C text."""
    i = code.find('\n{')
    if i < 0:
        return None
    head = code[:i]
    body = code[i + 1:]
    # the definition is the last non-empty chunk of head that mentions the name
    lines = head.rstrip().split('\n')
    k = len(lines) - 1
    while k >= 0 and name not in lines[k]:
        k -= 1
    if k < 0:
        return None
    # Ghidra puts a long definition's return type on its own line
    if lines[k].lstrip().startswith(name) and k > 0 and lines[k - 1].strip() and \
            not lines[k - 1].strip().startswith(('/*', '//', '#')):
        k -= 1
    defn = '\n'.join(lines[k:]).strip()
    pre = '\n'.join(lines[:k])
    return pre, defn, body


SMALL_RET = {'bool': 'uint', 'byte': 'uint', 'uchar': 'uint', 'undefined1': 'uint', 'undefined': 'uint',
             'char': 'int', 'sbyte': 'int', 'short': 'int', 'ushort': 'uint', 'word': 'uint',
             'undefined2': 'uint', 'wchar16': 'uint', 'BOOLEAN': 'uint', 'BYTE': 'uint', 'WORD': 'uint',
             'SHORT': 'int', 'CHAR': 'int', 'UCHAR': 'uint'}


def widen_return(defn, name):
    """Return values narrower than 32 bits are widened: functions are often
    called through `code *` pointers that read the whole register."""
    m = re.match(r'^(\s*)(\w+)(\s+)(' + re.escape(name) + r'\s*\()', defn)
    if m and m.group(2) in SMALL_RET:
        return m.group(1) + SMALL_RET[m.group(2)] + m.group(3) + defn[m.end(3):]
    return defn


def ptr_type(decl, name):
    """'T NAME[8]' -> 'T (*)[8]'  (type of a pointer to the declared object)."""
    # variable-length string types Ghidra declares without a size
    m = re.match(r'^(unicode|string|TerminatedCString|TerminatedUnicode|unicode32)\s+' + re.escape(name) + r'$', decl)
    if m:
        decl = {'unicode': 'wchar16', 'TerminatedUnicode': 'wchar16', 'unicode32': 'uint'}.get(m.group(1), 'char') \
            + ' ' + name + '[1]'
    i = decl.rfind(name)
    if i < 0:
        return None
    return decl[:i] + '(*)' + decl[i + len(name):]


class Gen:
    def __init__(self):
        self.funcs = load_export(os.path.join(EXPORT, 'functions.txt'))
        self.by_name = {f.name: f for f in self.funcs}
        self.overrides = set()
        if os.path.exists(OVERRIDES_FILE):
            for l in open(OVERRIDES_FILE):
                l = l.split('#')[0].strip()
                if l:
                    self.overrides.add(int(l.split()[0], 16))
        self.rename = {}           # identifier -> replacement
        self.imports = {}          # w32 name -> signature
        self.crt = {}              # crt name -> signature
        self.thunk_alias = {}      # internal thunk name -> target name
        self.game = []
        for f in self.funcs:
            if f.thunk:
                tname, ext, taddr = f.thunk
                if ext:
                    self.rename[f.name] = w32_name(f.name)
                    self.imports[w32_name(f.name)] = (f.name, f.sig)
                else:
                    self.thunk_alias[f.name] = tname
                continue
            if f.addr >= CRT_START:
                self.rename[f.name] = crt_name(f.name)
                self.crt[crt_name(f.name)] = f.sig
                continue
            if f.failed:
                report['failed'].append('%08x %s: %s' % (f.addr, f.name, f.failed))
                continue
            if 'bad instruction data' in f.code:
                report['bad_code'].append('%08x %s' % (f.addr, f.name))
                continue
            self.game.append(f)
        for a, t in self.thunk_alias.items():
            tf = self.by_name.get(t)
            if tf is not None and tf.addr >= CRT_START:
                self.rename[a] = crt_name(t)
            elif t in self.rename:
                self.rename[a] = self.rename[t]
            else:
                self.rename[a] = sanitize(t)
        for n, sig in EXTERNS.items():
            if n not in self.rename:
                self.rename[n] = w32_name(n)
            self.imports[w32_name(n)] = (n, sig)
        # Ghidra prints some special-character names already sanitized
        # (FID_conflict:_memcpy -> FID_conflict__memcpy)
        for n in list(self.rename):
            s2 = sanitize(n)
            if s2 != n and s2 not in self.rename:
                self.rename[s2] = self.rename[n]
        self.game_names = set(f.name for f in self.game)
        # special-character names must be replaced textually first
        self.special = sorted([n for n in self.rename if re.search(r'\W', n)], key=len, reverse=True)
        self.ident_re = re.compile(r'\b(' + '|'.join(re.escape(n) for n in sorted(
            (n for n in self.rename if not re.search(r'\W', n)), key=len, reverse=True)) + r')\b')

    # ------------------------------------------------------------------
    def rename_idents(self, text):
        for n in self.special:
            text = text.replace(n, self.rename[n])
        return self.ident_re.sub(lambda m: self.rename[m.group(1)], text)

    # pointer types whose targets may be unaligned (a table walked with an
    # `int *` in 39-byte steps): through these GCC never uses the ARM loads
    # that need alignment (LDRD/LDM/VLDR).  recomp_unaligned.h
    UNALIGNED_PTRS = [
        (r'\bint \*', 'recomp_s32u *'),
        (r'\b(?:uint|undefined4|dword|DWORD|UINT) \*', 'recomp_u32u *'),
        (r'\bshort \*', 'recomp_s16u *'),
        (r'\b(?:ushort|undefined2|word|WORD) \*', 'recomp_u16u *'),
        (r'\bfloat \*', 'recomp_f32u *'),
        (r'\bdouble \*', 'recomp_f64u *'),
        (r'\b(?:longlong|ulonglong|undefined8) \*', 'recomp_u64u *'),
    ]

    def fix_types(self, text):
        text = re.sub(r'\bwchar_t\b', 'wchar16', text)
        for pat, rep in self.UNALIGNED_PTRS:
            text = re.sub(pat, rep, text)
        return text

    @staticmethod
    def unary_deref(n, text):
        # '*NAME' where '*' is the unary operator (not a multiplication) and not a call
        for m in re.finditer(r'\*\s*' + re.escape(n) + r'\b(?!\s*\))', text):
            i = m.start() - 1
            while i >= 0 and text[i] in ' \t':
                i -= 1
            prev = text[i] if i >= 0 else '\n'
            if prev in '(=,;!&|?:+-<>{}~\n' or text[max(0, i - 5):i + 1] == 'return':
                if prev == '(' and text[i - 1:i + 1] == '((' and False:
                    continue
                return True
        return False

    @staticmethod
    def usage_type(pt, n, text):
        """Adjust a global's pointer type to how this function uses it.

        The asm engine keeps values of different types in the same global
        "register" (DAT_004e949c...), so the symbol map type can disagree
        with the text.  pt is 'T (*)'; returns a possibly corrected pointer type."""
        e = re.escape(n)
        called = re.search(r'\(\s*\*\s*' + e + r'\s*\)\s*\(', text)
        indexed = re.search(r'(?<![\w&])' + e + r'\s*\[', text) or Gen.unary_deref(n, text)
        arith = re.search(r'(?<![\w&])' + e + r'\s*(\+\+|--|[+-]\s*(?![=]))', text)
        scalar = re.match(r'^\s*(undefined\d?|u?int|uint|int|dword|DWORD|LONG|ulong|long|HANDLE|HWND|UINT|BOOL|WPARAM|LPARAM)\s*\(\*\)\s*$', pt)
        is_code = re.match(r'^\s*code\s*\*\s*\(\*\)\s*$', pt)
        if called and not is_code and '(' not in pt.replace('(*)', ''):
            if indexed or arith:
                return 'uchar *(*)'     # byte pointer that is sometimes called
            return 'code *(*)'
        if is_code and (indexed or arith):
            return 'uchar *(*)'
        if scalar and indexed:
            return 'undefined4 *(*)'
        return pt

    # ------------------------------------------------------------------
    @staticmethod
    def float_bits(rest, decls, f):
        """Ghidra prints a bit-for-bit CAST of a float to an integer type as
        `(int)x`, which C reads as a value conversion.  This compiler converts
        values with __ftol (or fistp -> ROUND()), so a plain integer cast of a
        float expression is a reinterpretation (`fstp dword [int slot]`, sign
        bit tests): emit it as recomp_fbits()."""
        fl = set()
        for d in decls:
            m = re.match(r'\s*(?:float|double|float10)\s+(\w+)\s*;', d)
            if m:
                fl.add(m.group(1))
        for l in f.locals:
            if re.match(r'\s*(?:float|double|float10)\s+\w+\s*;?\s*$', l.decl or ''):
                fl.add(l.name)
        out, i = [], 0
        pat = re.compile(r'\((int|uint|undefined4)\)\s*(\w+|\()')
        while True:
            m = pat.search(rest, i)
            if not m:
                out.append(rest[i:])
                break
            out.append(rest[i:m.start()])
            tok = m.group(2)
            if tok in fl and not rest[m.end():].lstrip().startswith(('[', '.', '->')):
                out.append('(%s)recomp_fbits(%s)' % (m.group(1), tok))
                i = m.end()
                continue
            if tok == '(':
                depth, j = 1, m.end()
                while j < len(rest) and depth:
                    depth += {'(': 1, ')': -1}.get(rest[j], 0)
                    j += 1
                expr = rest[m.end() - 1:j]
                if re.search(r'(?<![\w.])\d+\.\d', expr) and 'ROUND' not in expr and '__ftol' not in expr \
                        and not re.search(r'\((float10|double|float)\)', expr[:12]):
                    out.append('(%s)recomp_fbits%s' % (m.group(1), expr))
                    i = j
                    continue
            out.append(m.group(0))
            i = m.end()
        rest = ''.join(out)
        # the same without a cast: a float expression stored into a slot typed
        # as raw 4 bytes (`local_30[4] = local_28 / 128.0;` is `fstp dword`)
        u4p, u4 = set(), set()
        for d in decls:
            m = re.match(r'\s*undefined4\s+\*(\w+)\s*;', d)
            if m:
                u4p.add(m.group(1))
            m = re.match(r'\s*undefined4\s+(\w+)\s*;', d)
            if m:
                u4.add(m.group(1))
        for l in f.locals:
            if re.match(r'\s*undefined4\s+\*\w+\s*;?\s*$', l.decl or ''):
                u4p.add(l.name)
            elif re.match(r'\s*undefined4\s+\w+\s*;?\s*$', l.decl or ''):
                u4.add(l.name)

        def store(m):
            lv, base, idx, rhs = m.group(2), m.group(3), m.group(4), m.group(5)
            slot = (base in u4p and idx) or (base in u4 and not idx)
            floaty = re.search(r'(?<![\w.])\d+\.\d+', rhs) or \
                any(re.search(r'\b%s\b' % re.escape(v), rhs) for v in fl)
            if slot and floaty and not re.search(r'ROUND|ftol|\((?:int|uint|undefined4)\)|recomp_fbits', rhs):
                return '%s%s = recomp_fbits(%s);' % (m.group(1), lv, rhs)
            return m.group(0)
        return re.sub(r'(?m)^(\s*)((\w+)(\[[^\]\n]+\])?)\s*=\s*([^;=\n][^;\n]*);', store, rest)

    def emit_function(self, f):
        sp = split_def(f.code, f.name)
        if sp is None:
            report['no_def'].append('%08x %s' % (f.addr, f.name))
            return None, None
        pre, defn, body = sp
        defn = widen_return(CC_KEYWORDS.sub('', defn), f.name)
        varargs = defn.rstrip().endswith('...)')
        lines = body.split('\n')
        # lines[0] == '{' ; declarations until the first blank line
        decl_end = 1
        decl_re = re.compile(r'^\s+[A-Za-z_][\w ]*?[\s\*]+\(?\*?\w+\)?(\s*\[[^\]]*\])*\s*;\s*$')
        while decl_end < len(lines) and lines[decl_end].strip() != '' and decl_re.match(lines[decl_end]) \
                and '=' not in lines[decl_end] and not lines[decl_end].strip().startswith(('return', 'goto')):
            decl_end += 1
        decls = lines[1:decl_end]
        rest = '\n'.join(lines[decl_end:])
        rest = self.float_bits(rest, decls, f)
        # an integer->float conversion of an 8-byte value is `fild qword`,
        # which is signed: CONCAT44 is unsigned (ulonglong) here
        rest = re.sub(r'\((float10|double|float)\)CONCAT44\(', r'(\1)(longlong)CONCAT44(', rest)
        # float / double / 64-bit dereferences may be unaligned (they fault
        # on ARM): recomp_unaligned.h
        rest = rest.replace('*(float *)', '*(recomp_f32u *)').replace('*(double *)', '*(recomp_f64u *)')
        rest = re.sub(r'\*\((?:longlong|ulonglong|undefined8) \*\)', '*(recomp_u64u *)', rest)
        # integers as well: one converted to float is loaded with VLDR
        rest = re.sub(r'\*\((?:uint|undefined4|dword|DWORD|UINT) \*\)', '*(recomp_u32u *)', rest)
        rest = re.sub(r'\*\((?:int|LONG) \*\)', '*(recomp_s32u *)', rest)
        rest = re.sub(r'\*\((?:ushort|undefined2|word|WORD) \*\)', '*(recomp_u16u *)', rest)
        rest = re.sub(r'\*\(short \*\)', '*(recomp_s16u *)', rest)

        macros = []
        undefs = []
        defined = set()
        ftypes = {}

        # ---- globals
        used = set(re.findall(r'[A-Za-z_][\w:]*', rest + '\n'.join(decls)))
        for g in f.globals:
            n = g.name
            if n in self.game_names or n in self.rename or g.addr is None:
                continue
            if n.startswith('FUN_'):
                continue
            mname = sanitize(n)
            if n not in used and mname not in used:
                continue
            if not (IMG_LO - 0x1000 <= g.addr < IMG_HI):
                report['global_outside_image'].append('%08x %s in %s' % (g.addr, n, f.name))
                continue
            pt = ptr_type(self.fix_types(g.decl), n)
            if pt is None:
                report['global_decl'].append('%s: %s' % (n, g.decl))
                continue
            if n.startswith('LAB_'):
                report['lab_ref'].append('%08x %s in %08x %s' % (g.addr, n, f.addr, f.name))
            pt = self.usage_type(pt, n, rest)
            if (f.addr, g.addr) in FAKE_ADDRS:
                # a plain number: &DAT_x must evaluate to x itself
                report['fake_addr'].append('%08x %s %s' % (f.addr, f.name, n))
                macros.append('#define %s (*(%s)(recomp_uptr)0x%08x)' % (mname, pt, g.addr))
            else:
                # a scalar at a misaligned address: unaligned-safe type (ARM)
                ua = {'int (*)': ('recomp_s32u (*)', 4), 'uint (*)': ('recomp_u32u (*)', 4),
                      'undefined4 (*)': ('recomp_u32u (*)', 4), 'dword (*)': ('recomp_u32u (*)', 4),
                      'float (*)': ('recomp_f32u (*)', 4), 'short (*)': ('recomp_s16u (*)', 2),
                      'ushort (*)': ('recomp_u16u (*)', 2), 'undefined2 (*)': ('recomp_u16u (*)', 2),
                      'double (*)': ('recomp_f64u (*)', 4), 'undefined8 (*)': ('recomp_u64u (*)', 4),
                      'longlong (*)': ('recomp_u64u (*)', 4), 'ulonglong (*)': ('recomp_u64u (*)', 4)}
                if pt in ua and g.addr % ua[pt][1]:
                    pt = ua[pt][0]
                macros.append('#define %s (*(%s)VA(0x%08x))' % (mname, pt, g.addr))
            undefs.append('#undef %s' % mname)
            defined.add(mname)
            ftypes[mname] = pt
            if mname != n:
                rest = rest.replace(n, mname)
        # globals the symbol map did not describe: assume 4 bytes, from the name
        for n in sorted(set(re.findall(r'\b(_?(?:DAT|PTR_DAT|PTR|BYTE|WORD|DWORD)_([0-9a-f]{8}))\b', rest))):
            name, a = n
            if name in defined or name in self.game_names or name in self.rename:
                continue
            addr = int(a, 16)
            if not (IMG_LO - 0x1000 <= addr < IMG_HI):
                continue
            t = self.usage_type('undefined4 (*)', name, rest)
            ftypes[name] = t
            macros.append('#define %s (*(%s)VA(0x%08x))' % (name, t, addr))
            undefs.append('#undef %s' % name)
            defined.add(name)
            report['global_guessed'].append('%08x %s in %s' % (addr, name, f.name))
        # absolute low addresses Ghidra names xRamNNNNNNNN
        for name, pfx, a in sorted(set(re.findall(r'\b(([a-z]{1,3})Ram([0-9a-f]{8}))\b', rest))):
            if name in defined:
                continue
            addr = int(a, 16)
            t = {'s': 'short', 'u': 'uint', 'i': 'int', 'c': 'char', 'b': 'byte', 'p': 'void *', 'f': 'float',
                 'pc': 'char *', 'pi': 'int *', 'ps': 'short *', 'pu': 'uint *', 'pp': 'void **', 'pb': 'byte *',
                 'us': 'ushort', 'uc': 'uchar', 'd': 'double', 'pf': 'float *'}.get(pfx, 'undefined4')
            if IMG_LO - 0x1000 <= addr < IMG_HI:
                macros.append('#define %s (*(%s *)VA(0x%08x))' % (name, t, addr))
            else:
                macros.append('#define %s (*(%s *)(recomp_uptr)0x%08x)' % (name, t, addr))
                report['absolute_address'].append('%08x %s in %s' % (addr, name, f.name))
            undefs.append('#undef %s' % name)
            defined.add(name)

        # ---- stack frame for address-taken / aggregate locals
        stack_locals = [l for l in f.locals if l.kind == 'L' and l.stack is not None and l.stack < 0]
        def is_aggr(l):
            return '[' in l.decl or not re.match(
                r'^(unsigned |signed )?(undefined\d?|u?int\d?|uint|int|u?short|u?char|char|byte|word|dword|u?long|ulong|longlong|ulonglong|float|double|float10|bool|code|BOOL|DWORD|HWND|HANDLE|LPVOID|UINT|HRESULT|LONG|WPARAM|LPARAM|\w+ ?\*+)\s', l.decl)
        frame = []
        declared_names = set(l.name for l in f.locals)
        # "_local_34": a wider access overlapping declared locals -> 4 bytes at -0x34
        for pre_, nm, off in sorted(set(re.findall(r'\b(_)((?:local|[a-z]{1,4}Stack)_([0-9a-f]+))\b', rest))):
            alias = '_' + nm
            if alias in declared_names:
                continue
            frame.append(type('L', (), dict(name=alias, stack=-int(off, 16), size=4,
                                             decl='undefined4 ' + alias, kind='L'))())
            declared_names.add(alias)
        for l in stack_locals:
            if is_aggr(l) or re.search(r'&' + re.escape(l.name) + r'\b', rest):
                frame.append(l)
        # ESP-relative addresses: register0x00000010 + -0xN is stack offset -N
        esp_offs = [-int(x, 16) for x in re.findall(r'register0x00000010\s*\+\s*-0x([0-9a-f]+)', rest)]
        if 'register0x00000010' in rest and not esp_offs:
            esp_offs = [-4]
        if frame or esp_offs:
            lo = min([l.stack for l in frame] + esp_offs)
            hi = max([l.stack + max(l.size, 1) for l in frame] + [o + 4 for o in esp_offs])
            # A pointer to one stack local may be used to reach its neighbours
            # (Ghidra splits structures into separate scalars), so once any
            # local lives in memory, all of them keep their original offsets.
            for l in stack_locals:
                if l not in frame:
                    frame.append(l)
            lo = min([l.stack for l in frame] + esp_offs)
            hi = max([l.stack + max(l.size, 1) for l in frame] + [o + 4 for o in esp_offs])
            # locals indexed below their start (acStack_20a + -0x1a) reach
            # memory under the lowest named local
            byname = dict((l.name, l) for l in frame)
            for nm, neg in re.findall(r'\b(\w+)\s*\+\s*-0x([0-9a-f]+)\b', rest):
                if nm in byname:
                    lo = min(lo, byname[nm].stack - int(neg, 16))
            for nm, neg in re.findall(r'\b(\w+)\s*\[\s*-(\d+|0x[0-9a-f]+)\s*\]', rest):
                if nm in byname:
                    lo = min(lo, byname[nm].stack - int(neg, 0) * 4)
            lo -= 64   # slack for pushes Ghidra models below the locals
            # the original frame reaches up to the saved registers / return
            # address at offset 0: a structure only partly known to Ghidra
            # (DDCAPS, DDSURFACEDESC...) may extend up to there
            hi = max(hi, 0)
            fsize = (hi - lo + 7) & ~7
            fnames = set(l.name for l in frame)
            newdecls = []
            for d in decls:
                m = re.match(r'\s*(.*?)\b(\w+)\s*(\[[^\]]*\])*\s*;\s*$', d)
                if m and m.group(2) in fnames:
                    continue
                newdecls.append(d)
            decls = ['  unsigned char _frame[%d] RECOMP_ALIGN8;' % fsize] + newdecls
            for l in frame:
                pt = ptr_type(self.fix_types(l.decl), l.name)
                macros.append('#define %s (*(%s)(_frame + %d))' % (l.name, pt, l.stack - lo))
                undefs.append('#undef %s' % l.name)
            if esp_offs or 'register0x00000010' in rest:
                macros.append('#define register0x00000010 ((recomp_uptr)(_frame + %d))' % (-lo))
                undefs.append('#undef register0x00000010')
                report['esp_relative'].append('%08x %s' % (f.addr, f.name))

        # ---- uninitialised register artefacts start at 0
        nd = []
        for d in decls:
            m = re.match(r'(\s*)(.*?)\b((?:extraout|unaff|in)_\w+)\s*;\s*$', d)
            if m and '[' not in d:
                if m.group(3) == 'unaff_FS_OFFSET' or m.group(3) == 'in_FS_OFFSET':
                    d = '%s%s%s = (void *)recomp_fs_dummy;' % (m.group(1), m.group(2), m.group(3))
                else:
                    d = '%s%s%s = 0;' % (m.group(1), m.group(2), m.group(3))
                    if not m.group(3).startswith('extraout_var') and m.group(3) != 'unaff_retaddr':
                        report['register_artifact'].append('%08x %s %s' % (f.addr, f.name, m.group(3)))
            nd.append(d)
        decls = nd

        # ---- varargs: &stack0x.. -> va_list
        if varargs:
            params = [l for l in f.locals if l.kind == 'P']
            params.sort(key=lambda l: (l.stack if l.stack is not None else 0))
            if params:
                last = params[-1].name
                rest = re.sub(r'&stack0x[0-9a-f]+', '_va', rest)
                rest = re.sub(r'&[a-z]{0,3}Stack[0-9a-f]{8}\b', '_va', rest)
                decls = [d for d in decls if not re.match(r'\s*\w+\s+[a-z]{0,3}Stack[0-9a-f]{8}\s*;', d)]
                decls.append('  va_list _va;')
                rest = '\n  va_start(_va, %s);' % last + rest
            else:
                report['varargs_noparams'].append('%08x %s' % (f.addr, f.name))

        # ---- partial accesses  x._N_M_
        rest = re.sub(r'\b(\w+(?:(?:\.|->)\w+|\[[^\[\]]*\])*)\._(\d+)_(\d+)_',
                      lambda m: 'PIECE%s(%s,%s)' % (m.group(3), m.group(1), m.group(2)), rest)
        # Ghidra's `code` is also a byte of code: (code)x casts and plain derefs of code*
        rest = re.sub(r'\(code\)', '(uchar)', rest)
        for nm, pt in ftypes.items():
            if re.match(r'^\s*code\s*\*\s*\(\*\)\s*$', pt):
                rest = re.sub(r'(?<![(\w])\*' + re.escape(nm) + r'\b(?!\s*\))', '*(uchar *)' + nm, rest)
            elif pt.startswith('uchar *'):
                rest = re.sub(r'\(\s*\*\s*' + re.escape(nm) + r'\s*\)\s*\(', '(*(code *)' + nm + ')(', rest)
        # locals declared code * but used as byte pointers
        nd = []
        for d in decls:
            m = re.match(r'(\s*)code \*(\w+);\s*$', d)
            if m:
                e = re.escape(m.group(2))
                if re.search(r'(?<![\w&])' + e + r'\s*(\[|\+\+|--|[+-]\s*(?!=))', rest):
                    d = '%suchar *%s;' % (m.group(1), m.group(2))
                    rest = re.sub(r'\(\s*\*\s*' + e + r'\s*\)\s*\(', '(*(code *)' + m.group(2) + ')(', rest)
            nd.append(d)
        decls = nd
        # jump-table data Ghidra could not turn into a switch
        def swdata(m):
            report['switch_table'].append('%08x %s %s' % (f.addr, f.name, m.group(2)))
            return '(*(undefined4 (*))VA(0x%s))' % m.group(2)
        rest = re.sub(r'\bswitchD_[0-9a-f]+::(switchdataD_([0-9a-f]{8}))', swdata, rest)
        # DAT_xxxxxxxx_K : the bytes from address+K
        rest = re.sub(r'\b(?:DAT|_DAT)_([0-9a-f]{8})_(\d+)\b',
                      lambda m: '(*(undefined4 *)VA(0x%x))' % (int(m.group(1), 16) + int(m.group(2))), rest)
        rest = re.sub(r'PIECE3\((\w+),(\d+)\)\s*=\s*([^;]+);',
                      lambda m: 'recomp_set3((uchar *)&(%s) + %s, %s);' % (m.group(1), m.group(2), m.group(3)), rest)

        # ---- a label must be followed by a statement
        rest = re.sub(r'((?:\bcase [^:\n]+|\bdefault|\b[A-Za-z_]\w*):)(\s*\n\s*\})', r'\1 ;\2', rest)

        # ---- address literals
        def lit(m):
            v = int(m.group(1), 16)
            if (f.addr, v) in FAKE_ADDRS:
                report['fake_addr'].append('%08x %s 0x%x' % (f.addr, f.name, v))
                return m.group(0)
            if IMG_LO <= v < IMG_HI:
                report['addr_literal'].append('%08x %s: 0x%x' % (f.addr, f.name, v))
                return 'VAI(0x%x)' % v
            return m.group(0)
        rest = re.sub(r'(?<![\w.])(?<!VA\()0x([0-9a-f]{6,8})\b', lit, rest)

        for pat in ('halt_baddata', 'switchD_', 'stack0x', 'in_stack_', 'swi(', 'LOCK()', 'UNLOCK()', 'unaff_retaddr'):
            if pat in rest:
                report['manual:' + pat].append('%08x %s' % (f.addr, f.name))

        text = '\n'.join([defn, '{'] + decls + ['  RECOMP_ENTER(0x%08x);' % f.addr]) + rest
        text = self.fix_types(self.rename_idents(text))
        # DDSE.dll protection: DdseFunc() returns a function table and the
        # arguments pushed for it really belong to the table function called
        # right after:  t = DdseFunc(a, b); (**(code **)(t + 0x14))();
        def ddse(m):
            report['ddse_call'].append('%08x %s' % (f.addr, f.name))
            return m.group(0)[:-2] + '(' + m.group(3) + ')'
        text = re.sub(r'(\w+) = (\([^()]*\))?w32_DdseFunc\(([^;]*)\);\s*'
                      r'(?:\(\*\*\(code \*\*\)\(\1 \+ (?:0x[0-9a-f]+|\d+)\)\)|\(\*\(code \*\)\*\1\))\(\)', ddse, text)
        proto = self.fix_types(self.rename_idents(defn)).strip()
        out = []
        if pre.strip():
            out.append(pre.strip())
        out += macros
        out.append(text.rstrip())
        out += undefs
        return '\n'.join(out) + '\n', proto

    # ------------------------------------------------------------------
    def run(self):
        os.makedirs(OUT, exist_ok=True)
        protos = []
        files = []
        cur = []
        emitted = []
        for f in self.game:
            if f.addr in self.overrides:
                defn = split_def(f.code, f.name)
                if defn:
                    protos.append(self.fix_types(self.rename_idents(widen_return(CC_KEYWORDS.sub('', defn[1]), f.name))).strip() + ';')
                emitted.append(f)
                continue
            text, proto = self.emit_function(f)
            if text is None:
                continue
            protos.append(proto + ';')
            emitted.append(f)
            cur.append('/* %08x */\n%s' % (f.addr, text))
            if len(cur) >= PER_FILE:
                files.append(cur)
                cur = []
        if cur:
            files.append(cur)

        hdr = ('/* generated by recomp/gen.py - do not edit */\n' + MOD['prelude'] +
               '#include "recomp.h"\n#include "types.h"\n#include "w32_protos.h"\n'
               '#include "crt.h"\n#include "protos.h"\n#include <stdarg.h>\n\n')
        import glob as _glob
        for old in _glob.glob(os.path.join(OUT, MOD['prefix'] + '*.c')):
            os.remove(old)
        for i, fl in enumerate(files):
            with open(os.path.join(OUT, MOD['prefix'] + '%02d.c' % i), 'w', newline='\n') as w:
                w.write(hdr + '\n'.join(fl))
        with open(os.path.join(OUT, 'protos.h'), 'w', newline='\n') as w:
            w.write('/* generated by recomp/gen.py - do not edit */\n#ifndef GEN_PROTOS_H\n#define GEN_PROTOS_H\n')
            w.write('\n'.join(protos) + '\n')
            for a, t in sorted(self.thunk_alias.items()):
                if a not in self.rename or self.rename[a] == sanitize(t):
                    w.write('#define %s %s\n' % (sanitize(a), sanitize(t)))
            w.write('#endif\n')
        with open(os.path.join(OUT, 'w32_protos.h'), 'w', newline='\n') as w:
            w.write('/* generated by recomp/gen.py - do not edit */\n#ifndef GEN_W32_PROTOS_H\n#define GEN_W32_PROTOS_H\n')
            for n, (orig, sig) in sorted(self.imports.items()):
                if n in EXTERN_SIGS:
                    w.write(EXTERN_SIGS[n].replace('%', n) + ';\n')
                    continue
                if sig.startswith('undefined ') and sig.endswith('(void)'):
                    w.write('int %s();  /* signature unknown to Ghidra */\n' % n)
                    continue
                if not sig:
                    w.write('/* %s: no signature */\n' % n)
                    continue
                s = CC_KEYWORDS.sub('', sig)
                i = s.find(orig + '(')
                if i < 0:
                    i = s.find(orig)
                s = s[:i] + n + s[i + len(orig):]
                w.write(self.fix_types(s) + ';\n')
            w.write('#endif\n')
        with open(os.path.join(OUT, 'functable.c'), 'w', newline='\n') as w:
            w.write('/* generated by recomp/gen.py - do not edit */\n' + MOD['prelude'] + '#include "recomp.h"\n#include "types.h"\n'
                    '#include "w32_protos.h"\n#include "crt.h"\n#include "protos.h"\n\n'
                    'typedef struct { uint va; void *fn; } FuncEntry;\n'
                    'const FuncEntry %s[] = {\n' % MOD['table'])
            n = 0
            for f in sorted(emitted, key=lambda f: f.addr):
                w.write('    { 0x%08x, (void *)%s },\n' % (f.addr, sanitize(f.name)))
                n += 1
            for th in sorted((f for f in self.funcs if f.thunk and not f.thunk[1]), key=lambda f: f.addr):
                pass
            w.write('};\nconst int %s_count = %d;\n' % (MOD['table'], n))
        self.write_types()
        with open(os.path.join(OUT, 'report.txt'), 'w') as w:
            for k in sorted(report):
                w.write('== %s (%d)\n' % (k, len(report[k])))
                for l in report[k]:
                    w.write('  %s\n' % l)
        with open(os.path.join(OUT, 'crt_used.txt'), 'w') as w:
            for n, s in sorted(self.crt.items()):
                w.write('%s\t%s\n' % (n, s))
        print('emitted %d functions in %d files; %d imports, %d crt' % (
            len(emitted), len(files), len(self.imports), len(self.crt)))
        for k in sorted(report):
            print('  %-24s %d' % (k, len(report[k])))

    def write_types(self):
        from ctypes_order import reorder
        src = open(os.path.join(EXPORT, 'types.h'), encoding='utf-8', errors='replace').read()
        out = []
        skip = re.compile(r'^typedef\s+[\w\s\*]+?[\s\*]+(undefined\d?|bool|byte|dword|longlong|uchar|uint|ulong|ushort|wchar_t|word|qword|ulonglong|sbyte|ImageBaseOffset32|float10|pointer|pointer32|uint3|int3|string|va_list|size_t)\s*;\s*$')
        for line in src.split(NL):
            if skip.match(line) or line.strip() in ('float10',):
                continue
            out.append(line)
        txt = self.fix_types(NL.join(out))
        # GetProcAddress results are called with any arguments
        txt = re.sub(r'(typedef\s+\w+\s*\(\s*\*\s*(?:FARPROC|NEARPROC|PROC)\s*\))\s*\(void\)', r'\1()', txt)
        txt, dups = reorder(txt)
        for d in dups:
            report['type_duplicate'].append(str(d))
        with open(os.path.join(OUT, 'types.h'), 'w', newline=NL) as w:
            w.write('/* generated by recomp/gen.py from Ghidra data types - do not edit */' + NL +
                    '#ifndef GEN_TYPES_H' + NL + '#define GEN_TYPES_H' + NL + '#include "recomp.h"' + NL + NL)
            w.write(txt)
            w.write(NL + '#endif' + NL)


NL = chr(10)

if __name__ == '__main__':
    Gen().run()
