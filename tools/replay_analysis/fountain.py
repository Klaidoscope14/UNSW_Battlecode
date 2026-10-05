"""fountain.py LOG : fast-respawn tiles (period <= 5): pearls eaten there by each side, by phase,
and how the eaters ended (died in the pocket? cause)."""
import sys, collections
from ladderlog import Game, Map, MAPS, MAPDIR
import os
g = Game(sys.argv[1])
fast = set()
for l in open(os.path.join(MAPDIR, MAPS[g.name.lower()] + '.map')):
    p = l.split()
    if p and p[0] == 'TILE' and p[1] != 'COUNT' and p[3] != '0' and int(p[4]) <= 5:
        fast.add((int(p[1]), int(p[2])))
eat = collections.Counter(); eatph = collections.Counter(); eaters = collections.defaultdict(int)
alleat = collections.Counter()
def oe(gg, r, kind, i, data):
    if kind == 'move' and data['eat']:
        side = gg.side(i)
        alleat[side] += 1
        if data['to'] in fast:
            eat[side] += 1; eatph[(side, (r // 100) * 100)] += 1; eaters[(side, i)] += 1
g.run(onevent=oe)
print(f'{g.name}: {len(fast)} fast tiles; pearls eaten on them  us {eat["us"]}/{alleat["us"]}  opp {eat["opp"]}/{alleat["opp"]}')
print('  by 100-round phase:', sorted(eatph.items()))
for side in ('us', 'opp'):
    ids = [i for (s, i) in eaters if s == side]
    why = collections.Counter(g.deathwhy.get(i, 'alive') for i in ids)
    print(f'  {side}: {len(ids)} distinct eaters; their fate {dict(why)}')
