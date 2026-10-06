#!/usr/bin/env python3
"""compare.py NAME [NAME...] : summary line per results/NAME.txt (suite.sh output).
total W-L, queen deaths, protected maps (Maze, Slithery Fight, Default, Australia, Portals, Around UNSW, Autarky),
other maps, elimination wins, losses per map. Acceptance rule used so far: a change may lose at most one extra
protected-map game and must not raise queen deaths (noise is about +-3 wins / +-5 queen deaths per 204 games)."""
import re, sys, os, collections
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
PROT = {'maze', 'slithery_fight', 'default', 'australia', 'portals', 'unsw', 'autarky'}
for v in sys.argv[1:]:
    w = collections.Counter(); q = collections.Counter(); pm = collections.Counter(); how = collections.Counter()
    for l in open(os.path.join(R, v + '.txt')):
        m = re.search(r'/(\w+)\s+(\S+) [AB] s\d+: (\S) \(([^,)]*)', l)
        if not m: continue
        k = 'prot' if m.group(2) in PROT else 'other'
        w[(k, m.group(3))] += 1; q[k] += 'Qdead' in l; how[(m.group(3), m.group(4))] += 1
        if m.group(3) == 'L': pm[m.group(2)] += 1
    tw = w[('prot', 'W')] + w[('other', 'W')]; tl = w[('prot', 'L')] + w[('other', 'L')]
    print(f"{v:14} total {tw}-{tl} Qd {q['prot'] + q['other']} | protected {w[('prot','W')]}-{w[('prot','L')]} Qd {q['prot']} | "
          f"other {w[('other','W')]}-{w[('other','L')]} Qd {q['other']} | elim wins {how[('W','by elimination')]} | losses {dict(pm)}")
