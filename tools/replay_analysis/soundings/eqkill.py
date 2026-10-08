#!/usr/bin/env python3
"""Who killed their queen: for each enemy-queen death, our dragons that died head-on the same round (their last intent and
distance from her last head), and whether any other dragon of ours had her in view (Chebyshev 3) that round.
python3 eqkill.py DIR/out ..."""
import sys, glob, collections, re
for d in sys.argv[1:]:
    for tsv in sorted(glob.glob(d + '/*.tsv')):
        team, first = {}, {}
        for l in open(tsv[:-4] + '.map'):
            p = l.split()
            if p and p[0] == 'DRAGON': i = len(team); team[i] = int(p[1]); first.setdefault(int(p[1]), i)
        rows = [l.rstrip('\n').split('\t') for l in open(tsv) if l and not l.startswith('#')]
        votes = collections.Counter(); t2 = dict(team)
        for p in rows:
            if p[1] == 'split': t2[int(p[3])] = int(p[4])
            if p[1] == 'ind' and int(p[2]) in t2: votes[t2[int(p[2])]] += 1
        us = votes.most_common(1)[0][0]; eq = first[1 - us]
        name = open(tsv).readline().split('\t')[1][9:].strip()
        pos, ind, alive = {}, {}, set(team)
        byRound = collections.defaultdict(list)
        for p in rows:
            if p[1] == 'death': byRound[int(p[0])].append((int(p[2]), int(p[3])))
        for p in rows:
            R = int(p[0]); k = p[1]
            if k == 'split': team[int(p[3])] = int(p[4]); alive.add(int(p[3]))
            elif k == 'upd': pos[int(p[2])] = (int(p[3]), int(p[4]))
            elif k == 'ind': ind[int(p[2])] = p[3]
            elif k == 'death':
                i = int(p[2])
                if i == eq:
                    qx, qy = pos.get(eq, (0, 0))
                    ours = [(j, rs) for j, rs in byRound[R] if team.get(j) == us]
                    killers = [f"#{j} [{' '.join(ind.get(j, '?').split()[:4])}] d{abs(pos[j][0]-qx)+abs(pos[j][1]-qy) if j in pos else '?'}" for j, rs in ours if rs == 3]
                    watchers = [j for j in alive if team.get(j) == us and j in pos and j not in [x for x, _ in ours]
                                and max(abs(pos[j][0]-qx), abs(pos[j][1]-qy)) <= 3]
                    print(f"{name:16s} their queen r{R} {['wall','self','body','head-on','no-move'][int(p[3])]:7s} our head-on dead same round: {killers or '-'}; other of ours with her in view: {len(watchers)}")
                alive.discard(i)
