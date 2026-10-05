"""snap.py LOG ROUND [X Y RADIUS] : board at the START of ROUND (state after ROUND-1).
Whole map when no centre is given. Also lists living dragons (id, team, length, head, last status)."""
import sys
from ladderlog import Game, board, RX_LOG
g = Game(sys.argv[1]); R = int(sys.argv[2])
status = {}
def oe(gg, r, kind, i, data):
    pass
class Stop(Exception): pass
def onr(gg, r):
    if r == R:
        raise Stop
# capture statuses as the events stream
evs = g.events
for r, t in evs:
    if r >= R: break
    m = RX_LOG.match(t)
    if m: status[int(m.group(1))] = (r, m.group(2))
try:
    g.run(onround=onr)
except Stop:
    pass
m = g.map
if len(sys.argv) > 3:
    c = (int(sys.argv[3]), int(sys.argv[4])); rad = int(sys.argv[5]) if len(sys.argv) > 5 else 8
    print(board(g, c, rad))
else:
    # full map: centre with radius covering, print rows 0..H-1 directly
    import ladderlog
    occ = g.occupied(); out = []
    for y in range(m.H):
        top = '    '; row = f'{y:3d} '
        for x in range(m.W):
            top += ' ' + ('-' if m.kelp_edge(x, y, 'N') else 'P' if m.portal_edge(x, y, 'N') else ' ')
            ch = '.'
            if (x, y) in occ:
                j, k = occ[(x, y)]; ours = g.tm(j) == g.US
                ch = ('q' if j == g.US else 'k' if j == g.OPP else 'u' if ours else 'e')
                if k == 0: ch = ch.upper()
            elif (x, y) in g.pearls: ch = '*'
            row += ('|' if m.kelp_edge(x, y, 'W') else 'P' if m.portal_edge(x, y, 'W') else ' ') + ch
        out.append(top); out.append(row)
    print('\n'.join(out))
print(f'round {R} start; us={sum(1 for j in g.alive if g.tm(j)==g.US)} opp={sum(1 for j in g.alive if g.tm(j)==g.OPP)}')
for j in sorted(g.alive):
    if g.tm(j) == g.US:
        st = status.get(j, (0, ''))
        print(f'  us #{j:<4} L{len(g.body[j]):<3} head {g.body[j][0]}  [{st[0]}] {st[1]}')
