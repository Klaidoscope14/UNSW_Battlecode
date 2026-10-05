"""approach.py LOG : do heads move toward the other team's heads?  For every single-step move made
while an enemy head is within 5 (manhattan): share of moves that got closer / farther, vs the share
of legal-looking moves (non-kelp) that would have got closer (random baseline). Split by target type
(queen / non-queen) and by the mover's side."""
import sys, collections
from ladderlog import Game
g = Game(sys.argv[1])
pre = {}
stats = collections.defaultdict(lambda: [0, 0, 0, 0.0])   # closer, same, farther, baseline-closer
def onr(gg, r):
    pre.clear()
    heads = {j: gg.body[j][0] for j in gg.alive}
    for j, h in heads.items():
        best = None
        for k, h2 in heads.items():
            if gg.tm(k) == gg.tm(j): continue
            d = gg.map.dist(h, h2)
            if d <= 5 and (best is None or d < best[0]): best = (d, k, h2)
        if best: pre[j] = (h, best)
def oe(gg, r, kind, i, d):
    if kind != 'move' or d['of'] != 1 or i not in pre: return
    h, (d0, k, h2) = pre.pop(i)
    d1 = gg.map.dist(d['to'], h2)
    opts = [gg.map.nb(h[0], h[1], dd) for dd in 'NESW' if not gg.map.kelp_edge(h[0], h[1], dd)]
    base = sum(1 for o in opts if gg.map.dist(o, h2) < d0) / max(1, len(opts))
    key = (gg.side(i), 'vs queen' if k in (0, 1) else 'vs dragon', f'd{min(d0,5)}' if d0 <= 2 else 'd3-5')
    s = stats[key]; s[0 if d1 < d0 else 1 if d1 == d0 else 2] += 1; s[3] += base
g.run(onround=onr, onevent=oe)
print(g.name)
for key in sorted(stats):
    c, s0, f, b = stats[key]; n = c + s0 + f
    if n < 15: continue
    print(f'  {key[0]:<4} {key[1]:<10} {key[2]:<5} n={n:<5} closer {c/n:5.0%} (random {b/n:4.0%}, lift {c/max(b,1e-9):4.2f})  farther {f/n:5.0%}')
