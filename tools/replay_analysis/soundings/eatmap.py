import sys, collections
# eatmap.py TSV MAP RMAX : pearls eaten per column band by team (team of the dragon whose turn it was), and pearl spawns per band
tsv, mapf, rmax = sys.argv[1], sys.argv[2], int(sys.argv[3])
mp = open(mapf).read().split('\n'); W, H = map(int, mp[0].split()[1:3])
team = {}; starts = []
for l in mp:
    p = l.split()
    if p and p[0] == 'DRAGON': team[len(team)] = int(p[1]); starts.append((int(p[1]), int(p[3]), int(p[4])))
us = starts[0][0] if False else None
ours = [s for s in starts if s[0] == 0]; ourX = sum(s[1] for s in ours) / len(ours)
band = lambda x: min(7, int(x * 8 / W))
eat = {0: collections.Counter(), 1: collections.Counter()}; spawn = collections.Counter(); dead = set()
ind_team = collections.Counter()
for l in open(tsv):
    p = l.rstrip('\n').split('\t')
    if len(p) < 3 or p[0].startswith('#'): continue
    R = int(p[0])
    if R > rmax: break
    if p[1] == 'split': team[int(p[3])] = int(p[4])
    elif p[1] == 'ind': ind_team[team.get(int(p[2]), -1)] += 1
    elif p[1] == 'tile':
        x, y, pr, t = int(p[2]), int(p[3]), int(p[4]), int(p[5])
        if pr == 1: spawn[band(x)] += 1
        elif t >= 0 and t in team: eat[team[t]][band(x)] += 1
me = max(ind_team, key=ind_team.get)  # the team whose bot writes our indicators
side = 'left' if (ourX < W / 2) == (me == 0) else 'right'
print(f"  we are team {me}, our start on the {'left' if (ourX < W/2) == (me == 0) else 'right'}; columns in 8 bands of {W/8:.0f}")
print("  pearls appearing: " + ' '.join(f"{spawn[b]:4d}" for b in range(8)))
print("  eaten by us:      " + ' '.join(f"{eat[me][b]:4d}" for b in range(8)))
print("  eaten by them:    " + ' '.join(f"{eat[1-me][b]:4d}" for b in range(8)))
