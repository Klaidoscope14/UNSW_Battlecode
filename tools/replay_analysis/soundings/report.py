#!/usr/bin/env python3
"""Ladder report from decoded replays (dump.mjs output).  python3 report.py OUTDIR > report.md

Sections follow the Soundings site (results, by map, where length goes, pearl sources, length and dragons over
the game, when games turn, tactics) plus our own opening checks (who eats where, what our dragons were doing,
walled starts). "Us" is the team whose bot writes indicator strings."""
import sys, glob, json, re, collections, statistics

CAUSES = ['hit kelp/wall', 'hit own body', 'hit another body', 'head-on', 'no valid move']
MARKS = [25, 50, 75, 100, 150, 200, 250, 300, 350, 400, 450, 500]
TURN_MARKS = [50, 100, 200, 300]


def mean(v):
    return sum(v) / len(v) if v else 0.0


def load_map(path):
    lines = open(path).read().split('\n')
    W, H = map(int, lines[0].split()[1:3])
    name = next((l[9:] for l in lines if l.startswith('MAP_NAME')), '?')
    drag = []
    for l in lines:
        p = l.split()
        if p and p[0] == 'DRAGON':
            t, n = int(p[1]), int(p[2])
            cells = [(int(p[3 + 2 * i]), int(p[4 + 2 * i])) for i in range(n)]
            drag.append((t, n, cells))
    return W, H, name, drag


def game(tsv):
    W, H, name, drag = load_map(tsv[:-4] + '.map')
    rows = open(tsv).read().split('\n')
    res = json.loads(rows[2].split('\t')[1])
    team, L, alive = {}, {}, set()
    queen = {}
    for i, (t, n, cells) in enumerate(drag):
        team[i], L[i] = t, n
        alive.add(i)
        queen.setdefault(t, i)
    qstart = {t: drag[queen[t]][2][0] for t in queen}
    ind = collections.Counter()
    for r in rows[3:]:
        p = r.split('\t')
        if len(p) > 2 and p[1] == 'ind':
            ind[int(p[2])] += 1
    us_votes = collections.Counter()
    for i, c in ind.items():
        if i in team:
            us_votes[team[i]] += c
    us = us_votes.most_common(1)[0][0] if us_votes else 0
    them = 1 - us
    S = (us, them)
    g = dict(map=name, W=W, H=H, us=us, res=res, won=(res['winner'] == ('A' if us == 0 else 'B')),
             end=res['endReason'], rounds=0)
    lost = {t: collections.Counter() for t in S}
    dlost = {t: collections.Counter() for t in S}
    eaten = {t: collections.Counter() for t in S}      # by source: natural / own / enemy corpses
    eatenR = {t: {} for t in S}                           # mark -> eaten so far
    splits = {t: 0 for t in S}; splits100 = {t: 0 for t in S}
    sprints = {t: 0 for t in S}; paid = {t: 0 for t in S}; sonar = {t: 0 for t in S}
    src = {}                                              # cell -> source of the pearl lying there
    timeline = {t: {} for t in S}                         # mark -> (total length, dragons, longest, queen)
    peakN = {t: 0 for t in S}; peakL = {t: 0 for t in S}
    ahead = {}
    bands = {t: collections.Counter() for t in S}; spawnBands = collections.Counter()
    why = collections.Counter(); early_deaths = collections.Counter()
    far = set()                                           # our non-queen dragons that got 8+ from our queen's start by r60
    ourIds = set()
    lastDeath = None
    pending = []                                          # deaths this round: (id, team, reason)
    R = 0
    flip = qstart[us][0] > W / 2                          # bands read from our side

    def band(x):
        b = min(7, int(x * 8 / W))
        return 7 - b if flip else b

    def snapshot(r):
        st = {}
        for t in S:
            mine = [i for i in alive if team.get(i) == t]
            tot = sum(L[i] for i in mine)
            longest = max((L[i] for i in mine), default=0)
            ql = L[queen[t]] if queen[t] in alive else 0
            st[t] = (tot, len(mine), longest, ql)
            peakN[t] = max(peakN[t], len(mine)); peakL[t] = max(peakL[t], tot)
        return st

    def close_round(r):
        # head-on partners: same round, reason 3
        ho = [(i, t) for i, t, rs in pending if rs == 3]
        for i, t, rs in pending:
            c = CAUSES[rs] if 0 <= rs < 5 else '?'
            if rs == 3:
                c = 'head-on enemy' if any(t2 != t for j, t2 in ho if j != i) else 'head-on teammate'
            lost[t][c] += L.get(i, 0); dlost[t][c] += 1
            if t == us and r <= 150:
                early_deaths[c] += 1
        pending.clear()
        st = snapshot(r)
        for m in MARKS:
            if m == r:
                for t in S:
                    timeline[t][m] = st[t]
        if r in TURN_MARKS:
            key = lambda s: (s[3], s[2], s[0])
            ahead[r] = key(st[us]) > key(st[them])
            ahead[-r] = (st[us][2], st[us][0]) > (st[them][2], st[them][0])  # the Soundings site's rule
        for m in (50, 100, 150, 500):
            if m == r:
                for t in S:
                    eatenR[t][m] = sum(eaten[t].values())

    for r in rows[3:]:
        p = r.split('\t')
        if len(p) < 3 or not p[0].isdigit():
            continue
        rr = int(p[0]); k = p[1]
        while R < rr:
            close_round(R); R += 1
        if k != 'tile':
            lastDeath = None
        if k == 'upd':
            i = int(p[2])
            if team.get(i) == us and R <= 60 and i != queen[us]:
                hx, hy = int(p[3]), int(p[4]); qx, qy = qstart[us]
                if abs(hx - qx) + abs(hy - qy) >= 8:
                    far.add(i)
            if team.get(i) == us:
                ourIds.add(i)
        elif k == 'act':
            i = int(p[2]); t = team.get(i)
            if p[3] == 'move' and t in S:
                steps = int(p[4]); free = (L[i] + 3) // 4
                pd = max(0, steps - free)
                if pd:
                    sprints[t] += 1; paid[t] += pd; L[i] -= pd
        elif k == 'split':
            par, ch, t = int(p[2]), int(p[3]), int(p[4])
            team[ch] = t; L[ch] = int(p[5]); alive.add(ch)
            if len(p) > 6:
                L[par] = int(p[6])
            else:
                L[par] -= int(p[5])
            if t in S:
                splits[t] += 1
                if R <= 100:
                    splits100[t] += 1
        elif k == 'death':
            i, rs = int(p[2]), int(p[3]); t = team.get(i, -1)
            if t in S:
                pending.append((i, t, rs))
            alive.discard(i)
            lastDeath = t
        elif k == 'tile':
            x, y, pr, turn = int(p[2]), int(p[3]), int(p[4]), int(p[5])
            if pr == 1:
                src[(x, y)] = 'natural' if lastDeath is None else ('own' if lastDeath == us else 'enemy')
                if lastDeath is None and R <= 100:
                    spawnBands[band(x)] += 1
            else:
                t = team.get(turn)
                s = src.pop((x, y), 'natural')
                if t in S:
                    L[turn] = L.get(turn, 0) + 1
                    s2 = 'natural' if s == 'natural' else ('own corpses' if (s == 'own') == (t == us) else 'enemy corpses')
                    eaten[t][s2] += 1
                    if R <= 100 and s == 'natural':
                        bands[t][band(x)] += 1
        elif k == 'sonar':
            t = team.get(int(p[2]))
            if t in S:
                sonar[t] += 1
        elif k == 'ind':
            i = int(p[2])
            if team.get(i) == us and R <= 100:
                w = [x for x in p[3].split() if x not in ('QUEEN', 'LEAD', 'SCR', 'GRD') and not re.fullmatch(r'L\d+|s\d|P\d+\*?', x)]
                if len(w) >= 3:
                    why[w[2]] += 1
    close_round(R)
    g['rounds'] = R
    final = snapshot(R)
    g.update(lost=lost, dlost=dlost, eaten=eaten, eatenR=eatenR, splits=splits, splits100=splits100, sprints=sprints,
             paid=paid, sonar=sonar, timeline=timeline, peakN=peakN, peakL=peakL, ahead=ahead, bands=bands,
             spawnBands=spawnBands, why=why, early_deaths=early_deaths, final=final, them=them,
             far=len(far), ourEarly=len([i for i in ourIds if i != queen[us]]))
    return g


def main():
    games = [game(f) for f in sorted(glob.glob(sys.argv[1] + '/*.tsv'))]
    if not games:
        print('no games'); return
    P = print
    won = sum(g['won'] for g in games)
    P(f"# Ladder report: {len(games)} games, {won} won, {len(games) - won} lost\n")
    el_w = sum(1 for g in games if g['won'] and g['end'] == 'teamEliminated')
    el_l = sum(1 for g in games if not g['won'] and g['end'] == 'teamEliminated')
    P(f"Games last {mean([g['rounds'] for g in games]):.0f} rounds on average; {el_w} of the wins and {el_l} of the losses ended by elimination.\n")

    P("## By map\n")
    P("| Map | Result | End | Pearls eaten | Dragons lost | Splits | Peak dragons | Final total length | Final queen |")
    P("|---|---|---|---|---|---|---|---|---|")
    for g in sorted(games, key=lambda g: (g['won'], g['map'])):
        u, t = g['us'], g['them']
        P(f"| {g['map']} | {'W' if g['won'] else 'L'} | {'elim' if g['end'] == 'teamEliminated' else 'r500'} "
          f"| {sum(g['eaten'][u].values())} vs {sum(g['eaten'][t].values())} "
          f"| {sum(g['dlost'][u].values())} vs {sum(g['dlost'][t].values())} "
          f"| {g['splits'][u]} vs {g['splits'][t]} | {g['peakN'][u]} vs {g['peakN'][t]} "
          f"| {g['final'][u][0]} vs {g['final'][t][0]} | {g['final'][u][3]} vs {g['final'][t][3]} |")

    P("\n## Where length goes (length lost per game)\n")
    causes = ['hit kelp/wall', 'hit own body', 'hit another body', 'head-on enemy', 'head-on teammate', 'no valid move']
    P("| Cause | Us | Them |")
    P("|---|---|---|")
    for c in causes:
        P(f"| {c} | {mean([g['lost'][g['us']][c] for g in games]):.1f} | {mean([g['lost'][g['them']][c] for g in games]):.1f} |")
    tu = mean([sum(g['lost'][g['us']].values()) for g in games]); tt = mean([sum(g['lost'][g['them']].values()) for g in games])
    P(f"| **total** | **{tu:.1f}** | **{tt:.1f}** |")
    P(f"\nDragons lost per game: {mean([sum(g['dlost'][g['us']].values()) for g in games]):.1f} vs "
      f"{mean([sum(g['dlost'][g['them']].values()) for g in games]):.1f}. "
      "Self-inflicted deaths include deliberate feeding (ours) and 'no valid move' suicides (theirs).")

    P("\n## Pearls (per game)\n")
    P("| Source | Us | Them |")
    P("|---|---|---|")
    for s in ('natural', 'own corpses', 'enemy corpses'):
        P(f"| {s} | {mean([g['eaten'][g['us']][s] for g in games]):.1f} | {mean([g['eaten'][g['them']][s] for g in games]):.1f} |")
    eu = mean([sum(g['eaten'][g['us']].values()) for g in games]); et = mean([sum(g['eaten'][g['them']].values()) for g in games])
    P(f"| **total** | **{eu:.1f}** | **{et:.1f}** |")
    P(f"\nOur share of all pearls eaten: {100 * eu / max(1, eu + et):.0f}%.")

    P("\n## Over the game (mean of games still running)\n")
    P("| Round | Total length us | them | Dragons us | them |")
    P("|---|---|---|---|---|")
    for m in MARKS:
        gs = [g for g in games if m in g['timeline'][g['us']]]
        if len(gs) < 3:
            continue
        P(f"| {m} | {mean([g['timeline'][g['us']][m][0] for g in gs]):.0f} | {mean([g['timeline'][g['them']][m][0] for g in gs]):.0f} "
          f"| {mean([g['timeline'][g['us']][m][1] for g in gs]):.1f} | {mean([g['timeline'][g['them']][m][1] for g in gs]):.1f} |")

    P("\n## When games turn (ahead = queen length, then longest dragon, then total length)\n")
    P("| Round | Ahead, then won | Behind, then won |")
    P("|---|---|---|")
    for m in TURN_MARKS:
        a = [g for g in games if g['ahead'].get(m) is True]; b = [g for g in games if g['ahead'].get(m) is False]
        fa = f"{100 * mean([g['won'] for g in a]):.0f}% of {len(a)}" if a else '-'
        fb = f"{100 * mean([g['won'] for g in b]):.0f}% of {len(b)}" if b else '-'
        P(f"| {m} | {fa} | {fb} |")

    P("\nThe Soundings site's rule (longest dragon, then total length):\n")
    P("| Round | Ahead, then won | Behind, then won |")
    P("|---|---|---|")
    for m in TURN_MARKS:
        a = [g for g in games if g['ahead'].get(-m) is True]; b = [g for g in games if g['ahead'].get(-m) is False]
        fa = f"{100 * mean([g['won'] for g in a]):.0f}% of {len(a)}" if a else '-'
        fb = f"{100 * mean([g['won'] for g in b]):.0f}% of {len(b)}" if b else '-'
        P(f"| {m} | {fa} | {fb} |")

    P("\n## Tactics (per game)\n")
    P("| | Us | Them |")
    P("|---|---|---|")
    for lab, key in (('splits', 'splits'), ('splits by r100', 'splits100'), ('sprints (paid moves)', 'sprints'),
                     ('segments paid for sprints', 'paid'), ('peak dragons', 'peakN'), ('peak total length', 'peakL'),
                     ('sonar pings', 'sonar')):
        P(f"| {lab} | {mean([g[key][g['us']] for g in games]):.1f} | {mean([g[key][g['them']] for g in games]):.1f} |")

    P("\n## Opening, map by map (rounds 0-150)\n")
    P("Columns are 8 bands from our side (band 1) to theirs (band 8); natural pearls only, rounds 0-100.\n")
    for g in sorted(games, key=lambda g: (g['won'], g['map'])):
        u, t = g['us'], g['them']
        P(f"### {g['map']}: {'won' if g['won'] else 'lost'}\n")
        e = g['eatenR']
        P("| | r50 | r100 | r150 |")
        P("|---|---|---|---|")
        P(f"| pearls eaten us / them | {e[u].get(50, '-')} / {e[t].get(50, '-')} | {e[u].get(100, '-')} / {e[t].get(100, '-')} | {e[u].get(150, '-')} / {e[t].get(150, '-')} |")
        tl = g['timeline']
        P(f"| dragons us / them | " + ' | '.join(f"{tl[u][m][1]} / {tl[t][m][1]}" if m in tl[u] else '-' for m in (50, 100, 150)) + ' |')
        P(f"\n- Pearls appearing by band: `{' '.join(f'{g['spawnBands'][b]:3d}' for b in range(8))}`")
        P(f"- Eaten by us:              `{' '.join(f'{g['bands'][u][b]:3d}' for b in range(8))}`")
        P(f"- Eaten by them:            `{' '.join(f'{g['bands'][t][b]:3d}' for b in range(8))}`")
        P(f"- Splits by r100: {g['splits100'][u]} vs {g['splits100'][t]}. Our deaths r0-150: "
          + (', '.join(f'{c} {n}' for c, n in g['early_deaths'].most_common()) or 'none'))
        P(f"- Our dragons r0-100: " + ', '.join(f'{k} {v}' for k, v in g['why'].most_common(6)))
        if g['ourEarly'] and g['far'] == 0:
            P(f"- **No non-queen dragon of ours got 8+ cells from our queen's start by round 60** (walled in or stayed home).")
        notes = []
        if e[u].get(100, 0) * 2 < e[t].get(100, 0):
            notes.append(f"out-eaten {e[t].get(100, 0)} to {e[u].get(100, 0)} by round 100")
        mid = sum(g['spawnBands'][b] for b in (3, 4)); allp = sum(g['spawnBands'].values())
        if allp and mid > 0.5 * allp and sum(g['bands'][u][b] for b in (3, 4)) * 3 < sum(g['bands'][t][b] for b in (3, 4)):
            notes.append(f"{100 * mid // allp}% of opening pearls appear in the middle two bands and they take them")
        if notes:
            P(f"- **Flags:** " + '; '.join(notes))
        P("")


main()
