import sys, glob, collections, re, json, os
# econ5116.py DIR : per map opening economy, us = team 0 (A)
RS = ['our bed', 'their bed']
def load(tsv):
    mp = open(tsv[:-4] + '.map').read().split('\n')
    W, H = map(int, mp[0].split()[1:3])
    beds = set(); team = {}; starts = []
    for l in mp:
        p = l.split()
        if not p: continue
        if p[0] == 'TILE' and len(p) >= 5 and (p[3] != '0' or p[4] != '0'): beds.add((int(p[1]), int(p[2])))
        if p[0] == 'DRAGON': team[len(team)] = int(p[1]); starts.append((int(p[1]), int(p[2]), (int(p[3]), int(p[4]))))
    qs = {}
    for t, L, c in starts:  # the queen: longest? queens are ids 0/1
        pass
    return W, H, beds, team, starts, mp[2]
out = []
for tsv in sorted(glob.glob(sys.argv[1] + '/*.tsv')):
    W, H, beds, team, starts, name = load(tsv)
    lines = open(tsv).read().split('\n')
    res = json.loads(lines[2].split('\t')[1])
    qstart = {team[0]: starts[0][2], team[1]: starts[1][2]}
    def zone(x, y):
        a = abs(x - qstart[0][0]) + abs(y - qstart[0][1]); b = abs(x - qstart[1][0]) + abs(y - qstart[1][1])
        return 'ours' if a + 2 < b else ('theirs' if b + 2 < a else 'mid')
    eat = {m: collections.Counter() for m in (50, 100, 150, 500)}
    alive = {0: set(), 1: set()}
    for i, t in team.items(): alive[t].add(i)
    splits = collections.Counter(); deaths = collections.Counter(); deathZ = collections.Counter()
    alivemark = {}; why = collections.Counter(); bedSpawn = collections.Counter()
    elim = None
    for l in lines[3:]:
        p = l.split('\t')
        if len(p) < 3: continue
        R = int(p[0]); k = p[1]
        for m in (50, 100, 150):
            if R > m and m not in alivemark: alivemark[m] = (len(alive[0]), len(alive[1]))
        if k == 'split':
            team[int(p[3])] = int(p[4]); alive[int(p[4])].add(int(p[3]))
            if R <= 100: splits[int(p[4])] += 1
        elif k == 'death':
            d = int(p[2]); t = team.get(d, -1); alive[t].discard(d) if t >= 0 else None
            if t == 0 and R <= 150: deaths[['wall/kelp', 'self', 'other body', 'head-on', 'no move'][int(p[3])]] += 1
            if t == 0 and not alive[0] and elim is None: elim = R
        elif k == 'tile':
            x, y, pr, turn = int(p[2]), int(p[3]), int(p[4]), int(p[5])
            if pr == 1 and (x, y) in beds and R <= 150: bedSpawn[zone(x, y)] += 1
            if pr == 0 and turn >= 0:
                t = team.get(turn, -1); src = 'bed' if (x, y) in beds else 'drop'
                for m in (50, 100, 150, 500):
                    if R <= m: eat[m][(t, src, zone(x, y) if src == 'bed' else '')] += 1
        elif k == 'ind' and R <= 100 and team.get(int(p[2])) == 0:
            w = [x for x in p[3].split() if x not in ('QUEEN', 'LEAD', 'SCR', 'GRD') and not re.fullmatch(r'L\d+|s\d', x)]
            why[' '.join(w[2:3])] += 1
    def tot(m, t, src=None): return sum(v for (tt, s, z), v in eat[m].items() if tt == t and (src is None or s == src))
    def bz(m, t): return '/'.join(str(sum(v for (tt, s, z), v in eat[m].items() if tt == t and s == 'bed' and z == zz)) for zz in ('ours', 'mid', 'theirs'))
    win = 'W' if res['winner'] == 'A' else 'L'
    print(f"\n== {name[9:]:18s} {win}  {res['endReason']}{' (we were eliminated r%d)' % elim if elim else ''}")
    print(f"  beds spawned r0-150 by zone ours/mid/theirs: {bedSpawn['ours']}/{bedSpawn['mid']}/{bedSpawn['theirs']}")
    for m in (50, 100, 150):
        print(f"  r{m:<3} eaten us {tot(m,0):4d} (bed ours/mid/theirs {bz(m,0)}, drops {tot(m,0,'drop')})   them {tot(m,1):4d} (bed {bz(m,1)}, drops {tot(m,1,'drop')})   dragons {alivemark.get(m, ('-','-'))[0]} v {alivemark.get(m, ('-','-'))[1]}")
    print(f"  splits r0-100: us {splits[0]} them {splits[1]};  our deaths r0-150: {dict(deaths)}")
    print(f"  our dragons' intent r0-100: {', '.join(f'{k} {v}' for k, v in why.most_common(7))}")
