#!/usr/bin/env python3
"""same.py A B : identical-game check between results/A.txt and results/B.txt (same seeds). Pure refactors
must print IDENTICAL; otherwise lists every game whose result line differs."""
import re, sys, os
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
def load(v):
    d = {}
    for l in open(os.path.join(R, v + '.txt')):
        m = re.search(r'/(\w+)\s+(\S+ [AB] s\d+): (.*)$', l)
        if m: d[(m.group(1), m.group(2))] = m.group(3).strip()
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
diff = [k for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
print(f"IDENTICAL ({len(a)} games)" if not diff else f"{len(diff)} of {len(a)} games differ")
for k in diff[:40]: print(' ', k, '|', a.get(k), '|', b.get(k))
