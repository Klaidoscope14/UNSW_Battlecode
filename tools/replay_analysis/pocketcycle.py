"""pocketcycle.py LOG : every trip into a fast-respawn (fountain) pocket, per side:
entry length, fountain pearls eaten, splits made during the trip (child sizes), how it ended,
net length = children's cells that got out - entry length.  Trips end at the dragon's death or
when it is 3+ cells away from every fountain tile."""
import sys, os, collections
from ladderlog import Game, MAPS, MAPDIR
g = Game(sys.argv[1])
fast = set()
for l in open(os.path.join(MAPDIR, g.mapkey + '.map')):
    p = l.split()
    if p and p[0] == 'TILE' and p[1] != 'COUNT' and p[3] != '0' and int(p[4]) <= 5:
        fast.add((int(p[1]), int(p[2])))
trips = {}; done = []
def near(c):
    return min(g.map.dist(c, f) for f in fast) <= 2
def oe(gg, r, kind, i, d):
    if i is None: return
    if kind == 'move':
        t = trips.get(i)
        if d['eat'] and d['to'] in fast and t is None:
            t = trips[i] = {'side': gg.side(i), 'r': r, 'entry': d['len'] - 1, 'eat': 0, 'kids': [], 'end': None}
        if t is not None:
            if d['eat']: t['eat'] += 1
            if not near(d['to']):
                t['end'] = ('left', d['len']); done.append(trips.pop(i))
    elif kind == 'split' and i in trips:
        trips[i]['kids'].append(d['k'])
    elif kind == 'die' and i in trips:
        t = trips.pop(i); t['end'] = (d['why'], len(d['body'])); done.append(t)
g.run(onevent=oe)
for side in ('us', 'opp'):
    T = [t for t in done if t['side'] == side]
    if not T: print(f'{side}: no trips'); continue
    ent = collections.Counter(t['entry'] for t in T)
    net = []
    for t in T:
        out = sum(t['kids']) + (t['end'][1] if t['end'][0] == 'left' else 0)
        net.append(out - t['entry'])
    ends = collections.Counter(t['end'][0] for t in T)
    stub = collections.Counter(t['end'][1] for t in T if t['end'][0] != 'left')
    print(f'{side}: {len(T)} trips; entry length {dict(sorted(ent.items()))}; pearls/trip {sum(t["eat"] for t in T)/len(T):.2f}; '
          f'net length/trip {sum(net)/len(T):+.2f} (total {sum(net):+d}); ends {dict(ends)}; dead stub length {dict(sorted(stub.items()))}')
    kids = collections.Counter(tuple(t['kids']) for t in T)
    print(f'     split pattern in the pocket (child sizes): {kids.most_common(6)}')
