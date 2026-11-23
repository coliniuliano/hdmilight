#!/usr/bin/env python3
"""show.py RESOLVED ADDR|grep:REGEX|callers:ADDR ... : print functions of a tools/resolve.py output without local declarations."""
import re, sys
src = open(sys.argv[1]).read(); fs = {}
for f in re.split(r'(?=/\* ==== )', src):
    m = re.match(r'/\* ==== (\S+) @ ([0-9a-f]{8})\s+size (\d+)', f)
    if m: fs[int(m.group(2), 16)] = f
DECL = re.compile(r'^\s*(undefined\d?|int|uint|byte|char|ushort|short|bool|code|longlong|ulonglong|undefined)\s+\**\w+(\s*\[\d+\])*;$')
for a in sys.argv[2:]:
    if a.startswith('grep:'):
        for k, f in sorted(fs.items()):
            if re.search(a[5:], f): print('%08x %d lines' % (k, len(f.split('\n'))))
    elif a.startswith('callers:'):
        for k, f in sorted(fs.items()):
            if re.search(r'FUN_%s\b' % a[8:], f.split('\n', 2)[-1]): print('%08x calls %s' % (k, a[8:]))
    else:
        print('\n'.join(l for l in fs[int(a, 16)].rstrip().split('\n') if not DECL.match(l)))
