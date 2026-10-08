#!/usr/bin/env python3
"""Our queen's death in each game: round, cause, her length, her last intents, our/their dragons alive, the enemy heads
within 4 of her head the round she died (with their lengths), and who died with her.  python3 qdeath.py DIR/out ..."""
import sys, glob, re, collections

CAUSE = ['wall', 'self', 'body', 'head-on', 'no-move']
for d in sys.argv[1:]:
    for tsv in sorted(glob.glob(d + '/*.tsv')):
        team, L, first = {}, {}, {}
        for l in open(tsv[:-4] + '.map'):
            p = l.split()
            if p and p[0] == 'DRAGON':
                i = len(team); team[i] = int(p[1]); L[i] = int(p[2]); first.setdefault(int(p[1]), i)
        rows = [l.rstrip('\n').split('\t') for l in open(tsv) if l and not l.startswith('#')]
        name = open(tsv).readline().split('\t')[1][9:].strip()
        votes = collections.Counter()
        t2 = dict(team)
        for p in rows:
            if p[1] == 'split': t2[int(p[3])] = int(p[4])
            if p[1] == 'ind' and int(p[2]) in t2: votes[t2[int(p[2])]] += 1
        us = votes.most_common(1)[0][0]; q = first[us]
        pos, alive, intents = {}, {i for i in team}, collections.deque(maxlen=6)
        for p in rows:
            R = int(p[0]); k = p[1]
            if k == 'split':
                c, par = int(p[3]), int(p[2]); team[c] = int(p[4]); L[c] = int(p[5]); alive.add(c)
                if len(p) > 6: L[par] = int(p[6])
            elif k == 'upd': pos[int(p[2])] = (int(p[3]), int(p[4]))
            elif k == 'tile' and p[4] == '0' and int(p[5]) in L: L[int(p[5])] += 1
            elif k == 'act' and p[3] == 'move' and int(p[2]) in L:
                i = int(p[2]); paid = max(0, int(p[4]) - (L[i] + 3) // 4); L[i] -= paid
            elif k == 'ind' and int(p[2]) == q: intents.append(f"r{R}:" + ' '.join(p[3].split()[1:4]))
            elif k == 'death':
                i = int(p[2])
                if i == q:
                    hx, hy = pos.get(q, (0, 0))
                    near = sorted((abs(pos[j][0] - hx) + abs(pos[j][1] - hy), L.get(j, 0), j) for j in alive
                                  if j != q and team.get(j) != us and j in pos and abs(pos[j][0] - hx) + abs(pos[j][1] - hy) <= 4)
                    ours = sum(1 for j in alive if team.get(j) == us); theirs = sum(1 for j in alive if team.get(j) != us)
                    same = [f"#{p2[2]}(t{team.get(int(p2[2]))},{CAUSE[int(p2[3])]})" for p2 in rows if p2[0] == p[0] and p2[1] == 'death' and int(p2[2]) != q]
                    print(f"{name:16s} r{R} {CAUSE[int(p[3])]:7s} L{L.get(q)}  dragons {ours} v {theirs}  enemy heads within 4 (dist,len): "
                          f"{[(a, b) for a, b, _ in near][:5]}  died same round: {same[:4]}")
                    print(f"{'':16s} her last intents: {' | '.join(intents)}")
                alive.discard(i)
