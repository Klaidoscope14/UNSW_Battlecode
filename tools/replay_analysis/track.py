"""track.py LOG ID [FROM TO] : one dragon round by round (head, length, asks, status, death)."""
import sys, re
from ladderlog import Game, RX_LOG, RX_ASK, RX_DIE, RX_SPLIT
g = Game(sys.argv[1]); I = int(sys.argv[2])
lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0; hi = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9
rows = {}
def onr(gg, r):
    if I in gg.alive and lo <= r - 1 <= hi:
        rows.setdefault(r - 1, {})['pos'] = (gg.body[I][0], len(gg.body[I]))
g.run(onround=onr)
for r, t in g.events:
    if not (lo <= r <= hi): continue
    for rx, key in ((RX_LOG, 'st'), (RX_ASK, 'ask'), (RX_DIE, 'die')):
        m = rx.match(t)
        if m and int(m.group(1)) == I:
            rows.setdefault(r, {})[key] = t.split(' ', 2)[2] if key != 'st' else m.group(2)
    m = RX_SPLIT.match(t)
    if m and int(m.group(1)) == I:
        rows.setdefault(r, {})['split'] = t
print(f'#{I} team {"us" if g.tm(I)==g.US else "opp"} born {g.born.get(I)} died {g.died.get(I)} {g.deathwhy.get(I,"")}')
for r in sorted(rows):
    d = rows[r]
    print(f'r{r:<4} {str(d.get("pos","")):<18} {d.get("ask",""):<22} {d.get("st",""):<40} {d.get("split","")} {d.get("die","")}')
