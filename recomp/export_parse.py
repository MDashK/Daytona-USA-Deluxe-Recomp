"""Parser for the RecompExport.java output (functions.txt)."""
import re


class Local:
    __slots__ = ('where', 'size', 'kind', 'name', 'decl', 'stack')

    def __init__(self, where, size, kind, name, decl):
        self.where, self.size, self.kind, self.name, self.decl = where, size, kind, name, decl
        self.stack = int(where[6:]) if where.startswith('stack:') else None


class Global:
    __slots__ = ('addr', 'size', 'name', 'decl')

    def __init__(self, addr, size, name, decl):
        self.addr, self.size, self.name, self.decl = addr, size, name, decl


class Func:
    def __init__(self, addr, name):
        self.addr = addr
        self.name = name
        self.size = 0
        self.end = addr
        self.cc = ''
        self.sig = ''
        self.thunk = None          # (target_name, external?, target_addr)
        self.noreturn = False
        self.failed = None
        self.globals = []
        self.locals = []
        self.code = ''


EXTERNS = {}


def load_export(path):
    funcs = []
    EXTERNS.clear()
    cur = None
    code = None
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if code is not None:
                if line.startswith('@@END'):
                    cur.code = ''.join(code)
                    code = None
                    cur = None
                else:
                    code.append(line)
                continue
            line = line.rstrip('\n')
            if line.startswith('@@EXTERN '):
                nm, sig = line[9:].split(' :: ', 1)
                EXTERNS[nm] = sig
                continue
            if line.startswith('@@FUNC '):
                _, a, n = line.split(' ', 2)
                cur = Func(int(a, 16), n)
                funcs.append(cur)
            elif line.startswith('@@SIZE '):
                p = line.split()
                cur.size = int(p[1]); cur.end = int(p[2], 16)
            elif line.startswith('@@CC '):
                cur.cc = line[5:]
            elif line.startswith('@@SIG '):
                cur.sig = line[6:]
            elif line.startswith('@@THUNK '):
                p = line[8:].split()
                if len(p) >= 2:
                    cur.thunk = (p[0], p[1] == 'EXTERNAL', None if p[1] == 'EXTERNAL' else int(p[1], 16))
                else:
                    cur.thunk = (p[0] if p else '?', False, None)
            elif line.startswith('@@NORETURN'):
                cur.noreturn = True
            elif line.startswith('@@FAIL'):
                cur.failed = line[7:]
            elif line.startswith('@@GLOBAL '):
                head, decl = line[9:].split(' :: ', 1)
                a, sz, nm = head.split(' ', 2)
                cur.globals.append(Global(None if a == '?' else int(a, 16), int(sz), nm, decl))
            elif line.startswith('@@LOCAL '):
                head, decl = line[8:].split(' :: ', 1)
                w, sz, k, nm = head.split(' ', 3)
                cur.locals.append(Local(w, int(sz), k, nm, decl))
            elif line.startswith('@@CODE'):
                code = []
            elif line.startswith('@@END'):
                cur = None
    return funcs
