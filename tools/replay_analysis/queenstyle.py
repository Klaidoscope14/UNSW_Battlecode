"""queenstyle.py LOG : both queens' behaviour - length over time, distinct cells visited, bounding
box, share of rounds in a 2x2 loop (head cell repeats every 4 rounds), distance from start,
pearls eaten, own dragons within 3, enemy heads within 4."""
import sys, collections
from ladderlog import Game
g = Game(sys.argv[1])
st = {t: collections.defaultdict(list) for t in (0, 1)}
eaten = collections.Counter()
def onr(gg, r):
    occ = None
    for q in (0, 1):
        if q not in gg.alive: continue
        h = gg.body[q][0]; s = st[q]
        s['head'].append(h); s['len'].append(len(gg.body[q]))
        s['dstart'].append(gg.map.dist(h, gg.spawn[gg.tm(q)]))
        own = sum(1 for j in gg.alive if j != q and gg.tm(j) == gg.tm(q) and gg.map.dist(gg.body[j][0], h) <= 3)
        enm = sum(1 for j in gg.alive if gg.tm(j) != gg.tm(q) and gg.map.dist(gg.body[j][0], h) <= 4)
        s['own3'].append(own); s['enemy4'].append(enm)
def oe(gg, r, kind, i, d):
    if kind == 'move' and i in (0, 1) and d['eat']: eaten[i] += 1
g.run(onround=onr, onevent=oe)
for q in (0, 1):
    s = st[q]; H = s['head']
    if not H: continue
    who = 'US ' if g.tm(q) == g.US else 'OPP'
    loop = sum(1 for k in range(4, len(H)) if H[k] == H[k - 4]) / max(1, len(H) - 4)
    xs = [h[0] for h in H]; ys = [h[1] for h in H]
    med = lambda a: sorted(a)[len(a) // 2]
    lens = s['len']
    print(f'{g.name} {who} queen #{q}: alive {len(H)} rounds, died {g.died.get(q)}; length start {lens[0]} max {max(lens)} '
          f'r100 {lens[min(99,len(lens)-1)]} r250 {lens[min(249,len(lens)-1)]} end {lens[-1]}; pearls eaten {eaten[q]}')
    print(f'     distinct cells {len(set(H))}; 2x2-loop rounds {loop:.0%}; dist from start median {med(s["dstart"])} max {max(s["dstart"])}; '
          f'own within 3 median {med(s["own3"])}; enemy heads within 4: mean {sum(s["enemy4"])/len(H):.2f}, rounds with any {sum(1 for e in s["enemy4"] if e)/len(H):.0%}')
