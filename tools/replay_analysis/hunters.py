"""hunters.py LOG : our hunting effort vs their queen - per 50 rounds: our dragon-turns with a hunt
status, the closest any of our heads came to their queen, mean distance of our hunters to her,
and where she was (vs her start)."""
import sys, collections, re
from ladderlog import Game
g = Game(sys.argv[1]); Q = g.OPP
rows = collections.defaultdict(lambda: {'hunt': 0, 'min': 999, 'hd': [], 'qd': []})
def onr(gg, r):
    if Q not in gg.alive: return
    qh = gg.body[Q][0]; b = rows[(r - 1) // 50 * 50]
    b['qd'].append(gg.map.dist(qh, gg.spawn[gg.tm(Q)]))
    for j in gg.alive:
        if gg.tm(j) != gg.US: continue
        d = gg.map.dist(gg.body[j][0], qh); b['min'] = min(b['min'], d)
        st = gg.ind.get(j, '')
        if re.search(r'\b(hunt|hunt-queen|hunter|pack|lurk|regicide|pack-strike|intercept)\b', st):
            b['hunt'] += 1; b['hd'].append(d)
g.run(onround=onr)
print(f'{g.name}: their queen died {g.died.get(Q)}; mirror-spawn {g.spawn[g.tm(Q)]}')
for k in sorted(rows):
    b = rows[k]; hd = b['hd']
    print(f'  r{k:<4} hunter-turns {b["hunt"]:<5} mean hunter dist {sum(hd)/len(hd) if hd else 0:5.1f}  closest head {b["min"]:<4} queen from her start (max) {max(b["qd"]) if b["qd"] else "-"}')
