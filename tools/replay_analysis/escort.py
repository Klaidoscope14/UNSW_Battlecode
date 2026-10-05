"""escort.py LOG [QUEEN_SIDE=us] : enemy heads that stay within 2 cells of a queen's head for 3+
consecutive rounds ("escorts"), with the queen's open neighbour count each round and how it ended."""
import sys, collections
from ladderlog import Game
g = Game(sys.argv[1]); side = sys.argv[2] if len(sys.argv) > 2 else 'us'
Q = g.US if side == 'us' else g.OPP
hist = collections.defaultdict(list)   # enemy id -> [(round, dist)]
qtrace = []
def onr(gg, r):
    if Q not in gg.alive: return
    qh = gg.body[Q][0]
    occ = gg.occupied()
    free = 0
    for d in 'NESW':
        if gg.map.kelp_edge(qh[0], qh[1], d): continue
        n = gg.map.nb(qh[0], qh[1], d)
        if n not in occ or occ[n][0] == Q and occ[n][1] == len(gg.body[Q]) - 1: free += 1
    qtrace.append((r - 1, qh, len(gg.body[Q]), free))
    for j in gg.alive:
        if gg.tm(j) != gg.tm(Q):
            dd = gg.map.dist(gg.body[j][0], qh)
            if dd <= 2: hist[j].append((r - 1, dd))
g.run(onround=onr)
runs = []
for j, h in hist.items():
    cur = [h[0]]
    for a in h[1:]:
        if a[0] == cur[-1][0] + 1: cur.append(a)
        else:
            if len(cur) >= 3: runs.append((j, cur))
            cur = [a]
    if len(cur) >= 3: runs.append((j, cur))
qd = g.died.get(Q)
print(f'{g.name}: {side} queen died {qd} ({g.deathwhy.get(Q, "")}); escorts (enemy within 2 for 3+ rounds): {len(runs)}')
for j, cur in sorted(runs, key=lambda x: x[1][0][0]):
    endr = cur[-1][0]
    fate = 'killed queen' if qd and g.died.get(j) == qd and abs(endr - qd) <= 1 else f'enemy died r{g.died.get(j)}' if g.died.get(j) else 'enemy alive'
    print(f'  #{j:<5} r{cur[0][0]}-{endr} ({len(cur)} rounds) dists {"".join(str(d) for _, d in cur)}  -> {fate}')
