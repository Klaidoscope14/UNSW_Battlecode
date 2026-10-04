"""Round-by-round opening (default rounds 1-30): per side, eats / splits / deaths, and each dragon's path summary.
python3 opening.py <log.txt> [last_round]"""
import sys, collections
from ladderlog import Game
from analyze import our_reason


def run(path, last=30):
    g = Game(path); m = g.map
    per = collections.defaultdict(lambda: {'us': [], 'opp': []})
    track = collections.defaultdict(list)

    def onevent(gg, r, kind, i, d):
        if r > last:
            return
        s = g.side(i) if i is not None else None
        if kind == 'move':
            track[i].append((r, d['to'], 'E' if d['eat'] else ''))
            if d['eat']:
                per[r][s].append(f'#{i} eats@{d["to"]} L{d["len"]}' + (f' (step {d["step"]}/{d["of"]})' if d['of'] > 1 else ''))
        elif kind == 'split':
            per[r][s].append(f'#{i} split L{d["parentLen"]}->{d["parentLen"] - d["k"]}+{d["k"]} child #{d["child"]}')
        elif kind == 'die':
            per[r][s].append(f'#{i} DIES {d["why"]} L{len(d["body"])}')

    g.run(None, onevent)
    print(f'OPENING {g.opp_name} / {g.name}  (we are team {"A" if g.starter_us else "B"}; our queen #{g.US} starts {g.spawn[g.US]}, theirs #{g.OPP} {g.spawn[g.OPP]})')
    for r in sorted(per):
        u = '; '.join(per[r]['us']); o = '; '.join(per[r]['opp'])
        if u or o:
            print(f' r{r:<3} US: {u:<70} | OPP: {o}')
    print(' paths (start -> end, eats) of the starting dragons:')
    for i in sorted(t for t in track if t < len(g.map.dragons)):
        tr = track[i]
        eats = sum(1 for x in tr if x[2])
        print(f'   #{i:<3} {g.side(i):<3} {tr[0][1]} -> {tr[-1][1]}  steps {len(tr)}  eats {eats}  status now "{our_reason(g.ind.get(i, "")) if g.side(i) == "us" else ""}"')


if __name__ == '__main__':
    run(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 30)
