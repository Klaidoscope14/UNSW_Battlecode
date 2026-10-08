#!/usr/bin/env python3
"""Pearl races in rounds 0-RMAX: for each natural pearl, our and their nearest head (manhattan, wrap ignored) when it
appeared, and who ate it.  python3 races.py RMAX DIR/out ..."""
import sys, glob, collections
RMAX = int(sys.argv[1])
tot = collections.Counter(); per = collections.defaultdict(collections.Counter)
for d in sys.argv[2:]:
    for tsv in glob.glob(d + '/*.tsv'):
        team = {}
        for l in open(tsv[:-4] + '.map'):
            p = l.split()
            if p and p[0] == 'DRAGON': team[len(team)] = int(p[1])
        rows = [l.rstrip('\n').split('\t') for l in open(tsv) if l and not l.startswith('#')]
        name = open(tsv).readline().split('\t')[1][9:].strip()
        votes = collections.Counter(); t2 = dict(team)
        for p in rows:
            if p[1] == 'split': t2[int(p[3])] = int(p[4])
            if p[1] == 'ind' and int(p[2]) in t2: votes[t2[int(p[2])]] += 1
        us = votes.most_common(1)[0][0]
        pos, alive, open_ = {}, set(team), {}
        lastDeath = False
        for p in rows:
            R = int(p[0])
            if R > RMAX: break
            k = p[1]
            if k != 'tile': lastDeath = (k == 'death')
            if k == 'split': team[int(p[3])] = int(p[4]); alive.add(int(p[3]))
            elif k == 'death': alive.discard(int(p[2]))
            elif k == 'upd': pos[int(p[2])] = (int(p[3]), int(p[4]))
            elif k == 'tile':
                x, y, pr, turn = int(p[2]), int(p[3]), int(p[4]), int(p[5])
                if pr == 1 and not lastDeath:
                    du = min((abs(pos[i][0]-x)+abs(pos[i][1]-y) for i in alive if team.get(i) == us and i in pos), default=99)
                    dt = min((abs(pos[i][0]-x)+abs(pos[i][1]-y) for i in alive if team.get(i) != us and i in pos), default=99)
                    open_[(x, y)] = ('near' if du < dt else 'far' if du > dt else 'tie')
                elif pr == 0 and (x, y) in open_:
                    st = open_.pop((x, y)); who = 'us' if team.get(turn) == us else 'them'
                    tot[(st, who)] += 1; per[name][(st, who)] += 1
def row(c):
    return (f"we were nearer: ate {c[('near','us')]}, lost {c[('near','them')]} | tie: {c[('tie','us')]}/{c[('tie','them')]} | "
            f"they were nearer: we stole {c[('far','us')]}, they ate {c[('far','them')]}")
print("ALL: " + row(tot))
for m, c in sorted(per.items(), key=lambda kv: -kv[1][('near', 'them')]):
    print(f"  {m:16s} " + row(c))
