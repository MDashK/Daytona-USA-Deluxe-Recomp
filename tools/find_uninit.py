#!/usr/bin/env python3
"""Find callees whose return value is lost (Ghidra typed them void).

Compiles recomp/gen/*.c with the Vita GCC in WSL and -Wuninitialized; a
register variable read uninitialized right after a call means the callee
really returns something in EAX.  Writes recomp/uninit_calls.txt with one
"ret <addr> undefined4" line per callee, consumed by analyze_fixups.py.
"""
import os, re, subprocess, glob, collections

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
GEN = os.path.join(ROOT, 'recomp', 'gen')

def wsl(p):
    p = p.replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]

cmd = ('cd %s && export VITASDK=/home/mdashk/vitasdk-softfp; for f in recomp/gen/game_*.c; do '
       '$VITASDK/bin/arm-vita-eabi-gcc -c -O1 -fsigned-char -w -Wuninitialized -Wmaybe-uninitialized '
       '-Irecomp/include -Irecomp/gen $f -o /dev/null 2>&1 & done; wait') % wsl(ROOT)
cmd = cmd.replace(' -w ', ' ')
out = subprocess.run(['wsl', '-e', 'bash', '-lc', cmd], capture_output=True, text=True, errors='replace').stdout
warn = collections.defaultdict(set)
for m in re.finditer(r'recomp/gen/(game_\d+\.c):(\d+):\d+: warning: .(\w+). (?:is|may be) used uninitialized', out):
    warn[m.group(1)].add((int(m.group(2)), m.group(3)))
res = set()
report = []
for fn, items in warn.items():
    lines = open(os.path.join(GEN, fn), errors='replace').read().split('\n')
    for ln, var in sorted(items):
        if re.match(r'(local|[a-z]+Stack|extraout|unaff|in)_', var):
            continue
        # walk back to the previous statement that is a bare call
        for j in range(ln - 2, max(0, ln - 40), -1):
            s = lines[j].strip()
            if s.startswith('/*') and s.endswith('*/') and re.match(r'/\* [0-9a-f]{8} \*/', s):
                break
            m = re.match(r'^(FUN_([0-9a-f]{8}))\(.*\);$', s)
            if m:
                res.add(m.group(2))
                report.append('%s:%d %s after %s' % (fn, ln, var, m.group(1)))
                break
            if re.search(r'\b' + re.escape(var) + r'\s*=[^=]', s):
                break
with open(os.path.join(ROOT, 'recomp', 'uninit_calls.txt'), 'w') as w:
    for a in sorted(res):
        w.write('ret %s undefined4  # value used uninitialized after the call\n' % a)
print('%d warnings, %d callees' % (sum(len(v) for v in warn.values()), len(res)))
for r in report[:40]:
    print(' ', r)
