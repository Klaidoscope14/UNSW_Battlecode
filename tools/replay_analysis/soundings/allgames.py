#!/usr/bin/env python3
"""One line per game across ladder batches.  python3 allgames.py LABEL=DIR/out [LABEL=DIR/out ...]
Columns: version, map, result + how, pearls eaten by r100 (us/them), dragons at r150, our queen's fate, theirs, final
queen / longest / total length (us-them), and our queen's length when theirs died (if it did)."""
import sys, json, glob, collections

CAUSE = ['wall', 'self', 'body', 'head-on', 'no-move']


def one(label, tsv):
    team, first = {}, {}
    for l in open(tsv[:-4] + '.map'):
        p = l.split()
        if p and p[0] == 'DRAGON':
            i = len(team); team[i] = int(p[1]); first.setdefault(int(p[1]), i)
    lines = open(tsv).read().split('\n')
    name = lines[0].split('\t')[1][9:].strip()
    res = json.loads(lines[2].split('\t')[1])
    rows = [l.split('\t') for l in lines[3:] if l and not l.startswith('#')]
    votes = collections.Counter()
    for p in rows:
        if p[1] == 'split': team[int(p[3])] = int(p[4])
        if p[1] == 'ind' and int(p[2]) in team: votes[team[int(p[2])]] += 1
    us = votes.most_common(1)[0][0]; them = 1 - us
    q, eq = first[us], first[them]
    eat = collections.Counter(); alive = {0: set(), 1: set()}
    for i, t in list(team.items())[:len(first) * 3]:
        pass
    team2 = {}
    for l in open(tsv[:-4] + '.map'):
        p = l.split()
        if p and p[0] == 'DRAGON': team2[len(team2)] = int(p[1])
    for i, t in team2.items(): alive[t].add(i)
    d150 = None; qd = eqd = None; qlen = {}; qAtEq = None
    import re
    for p in rows:
        R = int(p[0])
        if R > 150 and d150 is None: d150 = (len(alive[us]), len(alive[them]))
        if p[1] == 'split': team2[int(p[3])] = int(p[4]); alive[int(p[4])].add(int(p[3]))
        elif p[1] == 'death':
            i = int(p[2]); t = team2.get(i)
            if t is not None: alive[t].discard(i)
            if i == q and qd is None: qd = f"r{R} {CAUSE[int(p[3])]}"
            if i == eq and eqd is None:
                eqd = f"r{R} {CAUSE[int(p[3])]}"; qAtEq = qlen.get(q)
        elif p[1] == 'tile' and p[4] == '0' and R <= 100 and int(p[5]) >= 0:
            eat[team2.get(int(p[5]))] += 1
        elif p[1] == 'ind' and int(p[2]) == q:
            m = re.search(r'L(\d+)', p[3])
            if m: qlen[q] = int(m.group(1))
    won = res['winner'] == ('A' if us == 0 else 'B')
    A, B = res['teamA'], res['teamB']
    U, T = (A, B) if us == 0 else (B, A)
    how = 'elim' if res['endReason'] == 'teamEliminated' else 'r500'
    return (label, name, 'W' if won else 'L', how, f"{eat[us]}/{eat[them]}", f"{d150[0] if d150 else '-'}/{d150[1] if d150 else '-'}",
            qd or 'alive', eqd or 'alive', f"{U['longestDragon']}-{T['longestDragon']}", f"{U['totalLength']}-{T['totalLength']}",
            '' if qAtEq is None or qd and eqd and int(qd.split()[0][1:]) < int(eqd.split()[0][1:]) else f"ours L{qAtEq} then {'died ' + qd if qd else 'lived'}")


out = []
for arg in sys.argv[1:]:
    label, d = arg.split('=')
    for f in sorted(glob.glob(d + '/*.tsv')):
        out.append(one(label, f))
out.sort(key=lambda r: (r[1], r[0]))
print('| ver | map | res | how | pearls r100 | dragons r150 | our queen died | their queen died | longest | total | after their queen died |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for r in out:
    print('| ' + ' | '.join(r) + ' |')
