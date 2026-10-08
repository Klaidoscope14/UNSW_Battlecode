#!/usr/bin/env python3
"""Play signature of both teams over many games (rounds 0-RMAX).  python3 signature.py RMAX DIR/out ...
bed watch: share of non-queen dragon-turns with the head within 2 of a natural pearl spot; eat rate: pearls per 100
dragon-turns; life: median rounds a non-queen dragon lives; own-body deaths and the share of a team's own death drops it
eats back within 15 rounds; queen: distance from her start, teammates within 3 of her, her length at r100/r200."""
import sys, glob, collections, statistics
RMAX = int(sys.argv[1])
agg = {s: collections.Counter() for s in ('us', 'them')}; life = {'us': [], 'them': []}; qlen = {'us': collections.defaultdict(list), 'them': collections.defaultdict(list)}
qdist = {'us': [], 'them': []}
for d in sys.argv[2:]:
    for tsv in glob.glob(d + '/*.tsv'):
        team, first, L = {}, {}, {}
        for l in open(tsv[:-4] + '.map'):
            p = l.split()
            if p and p[0] == 'DRAGON': i = len(team); team[i] = int(p[1]); first.setdefault(int(p[1]), i); L[i] = int(p[2])
        rows = [l.rstrip('\n').split('\t') for l in open(tsv) if l and not l.startswith('#')]
        votes = collections.Counter(); t2 = dict(team)
        for p in rows:
            if p[1] == 'split': t2[int(p[3])] = int(p[4])
            if p[1] == 'ind' and int(p[2]) in t2: votes[t2[int(p[2])]] += 1
        us = votes.most_common(1)[0][0]
        side = lambda t: 'us' if t == us else 'them'
        # natural pearl spots
        beds = set(); lastDeath = False
        for p in rows:
            if p[1] != 'tile': lastDeath = p[1] == 'death'
            elif p[4] == '1' and not lastDeath: beds.add((int(p[2]), int(p[3])))
        bedN = set()
        for (x, y) in beds:
            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    if abs(dx) + abs(dy) <= 2: bedN.add((x + dx, y + dy))
        start = {t: None for t in (0, 1)}; born = {i: 0 for i in team}; alive = set(team); pos = {}
        drops = []  # (round, cell, team that died)
        lastDeathT = None; curR = -1
        for p in rows:
            R = int(p[0]); k = p[1]
            if R > RMAX: break
            if R != curR:
                curR = R
                # per-round sampling
                for i in alive:
                    if i in pos and i not in first.values():
                        s = side(team.get(i)); agg[s]['turns'] += 1
                        if pos[i] in bedN: agg[s]['bedwatch'] += 1
                for t in (0, 1):
                    q = first[t]
                    if q in alive and q in pos:
                        if start[t] is None: start[t] = pos[q]
                        qdist[side(t)].append(abs(pos[q][0] - start[t][0]) + abs(pos[q][1] - start[t][1]))
                        agg[side(t)]['qturns'] += 1
                        agg[side(t)]['qescort'] += sum(1 for j in alive if j != q and team.get(j) == t and j in pos and abs(pos[j][0]-pos[q][0]) + abs(pos[j][1]-pos[q][1]) <= 3)
                        if R in (100, 200): qlen[side(t)][R].append(L.get(q, 0))
            if k != 'tile': lastDeathT = None
            if k == 'split':
                c, par = int(p[3]), int(p[2]); team[c] = int(p[4]); born[c] = R; alive.add(c); L[c] = int(p[5])
                if len(p) > 6: L[par] = int(p[6])
            elif k == 'upd': pos[int(p[2])] = (int(p[3]), int(p[4]))
            elif k == 'act' and p[3] == 'move' and int(p[2]) in L:
                i = int(p[2]); L[i] -= max(0, int(p[4]) - (L[i] + 3) // 4)
            elif k == 'death':
                i = int(p[2]); t = team.get(i); s = side(t)
                if i not in first.values(): life[s].append(R - born.get(i, 0))
                if p[3] == '1': agg[s]['selfdeaths'] += 1
                alive.discard(i); lastDeathT = t
            elif k == 'tile':
                x, y, pr, turn = int(p[2]), int(p[3]), int(p[4]), int(p[5])
                if pr == 1 and lastDeathT is not None: drops.append([R, (x, y), lastDeathT, None])
                if pr == 0 and turn in team:
                    agg[side(team[turn])]['eats'] += 1; L[turn] = L.get(turn, 0) + 1
                    for dr in drops:
                        if dr[3] is None and dr[1] == (x, y): dr[3] = (R, team[turn]); break
        for r0, cell, t, got in drops:
            s = side(t); agg[s]['drops'] += 1
            if got and got[1] == t and got[0] - r0 <= 15: agg[s]['ownback15'] += 1
            if got and got[1] != t and got[0] - r0 <= 15: agg[s]['stolen15'] += 1
for s in ('us', 'them'):
    a = agg[s]
    print(f"{s:5s} bed watch {100*a['bedwatch']/max(1,a['turns']):4.1f}%  eats/100 dragon-turns {100*a['eats']/max(1,a['turns']):4.1f}  "
          f"median life {statistics.median(life[s]) if life[s] else 0:5.1f}  own-body deaths {a['selfdeaths']}  own drops eaten back <=15r "
          f"{100*a['ownback15']/max(1,a['drops']):4.1f}% (enemy took {100*a['stolen15']/max(1,a['drops']):4.1f}%)  queen: dist from start "
          f"{statistics.mean(qdist[s]) if qdist[s] else 0:4.1f}, escorts within 3 {a['qescort']/max(1,a['qturns']):4.2f}, "
          f"length r100 {statistics.mean(qlen[s][100]) if qlen[s][100] else 0:4.1f} r200 {statistics.mean(qlen[s][200]) if qlen[s][200] else 0:4.1f}")
