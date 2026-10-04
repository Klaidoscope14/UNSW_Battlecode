"""Pearls reachable in one step (no kelp in between) at round start, and whether that dragon ate it.
python3 adjacent.py <log.txt>...   Breaks the misses down by the dragon's status (ours) and by threat."""
import sys, collections
from ladderlog import Game
from analyze import our_reason, reach


def run(path):
    g = Game(path); m = g.map
    res = {s: collections.Counter() for s in ('us', 'opp')}
    why = collections.Counter(); snap = []
    ate = {}

    def onround(gg, r):
        nonlocal snap
        for s, i, c, st, enemy_near in snap:
            ok = ate.get(i) == c
            res[s]['ate' if ok else 'missed'] += 1
            if s == 'us' and not ok:
                why[(st, 'enemy head within 2 of the pearl' if enemy_near else 'no enemy near')] += 1
        snap = []; ate.clear()
        pearls = g.pearls
        heads = [(g.body[j][0], g.side(j), len(g.body[j])) for j in g.alive]
        for i in g.alive:
            x, y = g.body[i][0]
            for d in 'NESW':
                if m.kelp_edge(x, y, d):
                    continue
                c = m.nb(x, y, d)
                if c in pearls:
                    s = g.side(i)
                    en = any(sd != s and m.dist(h, c) <= 2 for h, sd, L in heads)
                    snap.append((s, i, c, our_reason(g.ind.get(i, '')) if s == 'us' else '', en))
                    break

    def onevent(gg, r, kind, i, d):
        if kind == 'move' and d['eat'] and d['step'] == 1:
            ate[i] = d['to']

    g.run(onround, onevent)
    print(f'{g.opp_name[:10]:<10} {g.name:<18} one-step pearls eaten: us {res["us"]["ate"]}/{res["us"]["ate"] + res["us"]["missed"]}  '
          f'opp {res["opp"]["ate"]}/{res["opp"]["ate"] + res["opp"]["missed"]}')
    print('     our misses by status: ' + ', '.join(f'{k[0] or "?"}/{k[1][:5]} {v}' for k, v in why.most_common(10)))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        run(p)
