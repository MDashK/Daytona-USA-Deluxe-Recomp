#!/usr/bin/env python3
"""Show compiler errors from build_host/*.log with the offending source line.
usage: errs.py [substring] [max] [--names]"""
import re, sys, glob, os, collections

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
pat = args[0] if args else ''
n = int(args[1]) if len(args) > 1 else 20
names = '--names' in sys.argv
cache = {}
shown = 0
cnt = collections.Counter()
for log in sorted(glob.glob(os.path.join(ROOT, 'build_host', '*.log'))):
    for l in open(log, errors='replace'):
        m = re.match(r'(.*?)\((\d+)\): (?:fatal )?error (C\d+): (.*)', l.strip())
        if not m or pat not in m.group(3) + ' ' + m.group(4):
            continue
        if names:
            q = re.findall(r"'([^']*)'", m.group(4))
            if q:
                cnt[re.sub(r'[0-9a-f]{6,8}$', 'N', q[0])] += 1
            continue
        f, ln = m.group(1), int(m.group(2))
        if f not in cache:
            cache[f] = open(f, errors='replace').read().split('\n')
        src = cache[f][ln - 1].strip() if ln - 1 < len(cache[f]) else ''
        print('%s:%d %s | %s' % (os.path.basename(f), ln, m.group(4)[:90], src[:150]))
        shown += 1
        if shown >= n:
            sys.exit()
for k, v in cnt.most_common(40):
    print('%5d %s' % (v, k))
