import sys, glob, collections
# for each queen death: per round over the last 10, nearest enemy head distance (manhattan, no wrap) and her move
for d in sys.argv[1:]:
    for tsv in sorted(glob.glob(d + '/*.tsv')):
        team, first = {}, {}
        for l in open(tsv[:-4] + '.map'):
            p = l.split()
            if p and p[0] == 'DRAGON': i = len(team); team[i] = int(p[1]); first.setdefault(int(p[1]), i)
        rows = [l.rstrip('\n').split('\t') for l in open(tsv) if l and not l.startswith('#')]
        votes = collections.Counter()
        for p in rows:
            if p[1] == 'split': team[int(p[3])] = int(p[4])
            if p[1] == 'ind' and int(p[2]) in team: votes[team[int(p[2])]] += 1
        us = votes.most_common(1)[0][0]; q = first[us]
        qd = next((int(p[0]) for p in rows if p[1] == 'death' and int(p[2]) == q), None)
        if qd is None or qd < 20: continue
        pos = {}; alive = set(team); track = collections.OrderedDict(); ourNear = {}
        for p in rows:
            R = int(p[0])
            if R > qd: break
            if p[1] == 'split': alive.add(int(p[3]))
            if p[1] == 'death': alive.discard(int(p[2]))
            if p[1] == 'upd': pos[int(p[2])] = (int(p[3]), int(p[4]))
            if p[1] == 'upd' and int(p[2]) == q and R >= qd - 10:
                hx, hy = pos[q]
                ds = sorted(abs(pos[j][0]-hx)+abs(pos[j][1]-hy) for j in alive if team.get(j) != us and j in pos)
                ms = sorted(abs(pos[j][0]-hx)+abs(pos[j][1]-hy) for j in alive if team.get(j) == us and j != q and j in pos)
                track[R] = (ds[0] if ds else '-', sum(1 for x in ds if x <= 6), sum(1 for x in ms if x <= 3))
        name = open(tsv).readline().split('\t')[1][9:].strip()
        print(f"{name:16s} died r{qd}: (nearest enemy, enemies<=6, ours<=3) " + ' '.join(f"r{r}:{a}/{b}/{c}" for r, (a, b, c) in track.items()))
