"""Reorder Ghidra's DataTypeWriter output so a C compiler accepts it.

Ghidra writes types in no particular order: a struct may embed (by value)
a struct that is only defined later, some definitions appear twice, and
opaque types are empty structs.  This splits the header into top-level
statements, works out what each one needs complete (by-value uses) or only
declared (pointer uses), and emits them in dependency order.
"""
import re

KEYWORDS = set('''struct union enum typedef const volatile signed unsigned char short int long
float double void sizeof'''.split())


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def split_statements(src):
    out, depth, cur = [], 0, []
    for ch in src:
        cur.append(ch)
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        elif ch == ';' and depth == 0:
            st = ''.join(cur).strip()
            if st:
                out.append(st)
            cur = []
    rest = ''.join(cur).strip()
    if rest:
        out.append(rest)
    return out


def idents(s):
    return re.findall(r'[A-Za-z_]\w*', s)


class Stmt:
    def __init__(self, text):
        self.text = text
        self.provides = set()      # names this statement makes complete
        self.declares = set()      # names this statement makes known (maybe incomplete)
        self.need_complete = set()
        self.need_decl = set()
        self.key = None            # for duplicate detection


def field_needs(body, st):
    for fld in split_statements(body):
        fld = fld.rstrip(';').strip()
        if not fld:
            continue
        m = re.match(r'(struct|union|enum)\s+(\w+)\s*(.*)$', fld, re.S)
        if '(' in fld and re.search(r'\(\s*\*', fld):
            # function pointer: only declarations needed
            for t in idents(fld):
                st.need_decl.add(t)
            continue
        if m:
            tname = m.group(1) + ' ' + m.group(2)
            rest = m.group(3)
            if '*' in rest.split('[')[0]:
                pass  # pointer to struct: implicitly declared
            else:
                st.need_complete.add(tname)
            continue
        toks = idents(fld.split('[')[0])
        ptr = '*' in fld.split('[')[0]
        types = [t for t in toks[:-1] if t not in KEYWORDS]
        for t in types:
            (st.need_decl if ptr else st.need_complete).add(t)


def analyse(text):
    st = Stmt(text)
    t = text.rstrip(';').strip()
    # typedef struct X X, *PX;   (forward)
    m = re.match(r'typedef\s+(struct|union|enum)\s+(\w+)\s+([^{};]*)$', t, re.S)
    if m and '{' not in t:
        tag = m.group(1) + ' ' + m.group(2)
        names = [n.strip() for n in m.group(3).split(',')]
        for n in names:
            nm = n.lstrip('*').strip()
            st.declares.add(nm)
            if not n.startswith('*'):
                st.alias = getattr(st, 'alias', {})
                st.alias[nm] = tag
        st.declares.add(tag)
        st.key = ('fwd', t)
        st.kind = 'fwd'
        return st
    # struct X { ... }   /  union / enum
    m = re.match(r'(typedef\s+)?(struct|union|enum)\s+(\w+)\s*\{(.*)\}\s*([^;]*)$', t, re.S)
    if m:
        tag = m.group(2) + ' ' + m.group(3)
        st.provides.add(tag)
        st.declares.add(tag)
        if m.group(2) != 'enum':
            field_needs(m.group(4), st)
        if m.group(1) and m.group(5).strip():
            for n in m.group(5).split(','):
                nm = n.strip().lstrip('*').strip()
                st.declares.add(nm)
                if not n.strip().startswith('*'):
                    st.provides.add(nm)
        st.key = ('def', tag)
        st.kind = 'def'
        return st
    # typedef <type> NAME;  or function typedefs
    if t.startswith('typedef'):
        body = t[len('typedef'):].strip()
        fm = re.search(r'\(\s*\*\s*(\w+)\s*\)\s*\(', body) or re.search(r'(\w+)\s*\(', body)
        if fm and '(' in body:
            name = fm.group(1)
            st.declares.add(name)
            st.provides.add(name)
            for x in idents(body):
                if x != name:
                    st.need_decl.add(x)
        else:
            toks = idents(body.split('[')[0])
            name = toks[-1]
            st.declares.add(name)
            st.provides.add(name)
            ptr = '*' in body
            sm = re.match(r'(struct|union|enum)\s+(\w+)', body)
            if sm:
                tag = sm.group(1) + ' ' + sm.group(2)
                if ptr:
                    st.need_decl.add(tag)
                else:
                    st.need_complete.add(tag)
            else:
                for x in toks[:-1]:
                    if x not in KEYWORDS:
                        (st.need_decl if ptr else st.need_complete).add(x)
        st.key = ('typedef', tuple(sorted(st.declares)))
        st.kind = 'typedef'
        return st
    st.kind = 'other'
    st.key = ('other', t)
    return st


def reorder(src, builtin=()):
    src = strip_comments(src)
    src = re.sub(r'^\s*#.*$', '', src, flags=re.M)
    stmts = [analyse(s) for s in split_statements(src)]
    seen = {}
    uniq = []
    dups = []
    for s in stmts:
        if s.key in seen:
            if seen[s.key].text != s.text and s.kind == 'def':
                dups.append(s.key[1])
            continue
        # a second typedef of the same name with different text: keep the first
        if s.kind == 'typedef':
            nm = next(iter(s.declares))
            if ('tname', nm) in seen:
                dups.append(nm)
                continue
            seen[('tname', nm)] = s
        seen[s.key] = s
        uniq.append(s)
    known = set(builtin) | KEYWORDS
    defined_somewhere = set()
    for s in uniq:
        defined_somewhere |= s.declares | s.provides
    complete = set(builtin)
    declared = set(builtin)
    alias = {}
    out = []
    # forward declarations first
    fwd = [s for s in uniq if s.kind == 'fwd']
    for s in fwd:
        out.append(s.text)
        declared |= s.declares
        alias.update(getattr(s, 'alias', {}))
    rest = [s for s in uniq if s.kind != 'fwd']

    def is_complete(n):
        if n in complete or n not in defined_somewhere:
            return True
        if n in alias:
            return alias[n] in complete or alias[n] not in defined_somewhere
        return False

    def is_declared(n):
        return n in declared or n not in defined_somewhere or n in complete

    pending = rest
    while pending:
        progress = False
        nxt = []
        for s in pending:
            if all(is_complete(n) for n in s.need_complete) and all(is_declared(n) for n in s.need_decl):
                out.append(s.text)
                complete |= s.provides
                declared |= s.declares | s.provides
                progress = True
            else:
                nxt.append(s)
        if not progress:
            # cycle or missing type: emit the rest as they are
            for s in nxt:
                out.append(s.text)
            break
        pending = nxt
    text = '\n\n'.join(out)
    # empty aggregates are not valid C
    text = re.sub(r'((?:struct|union)\s+\w+\s*\{)\s*\}', r'\1 char _opaque; }', text)
    return text, dups
