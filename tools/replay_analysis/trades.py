"""trades.py LOG : our attack-type deaths (snipe/attack/hunt/pack/regicide/intercept/last-attack)
- did an enemy die in the same head-on (a 1-for-1 trade) or did we die alone; enemy length traded;
real fleet sizes at the time."""
import sys, collections, re
from ladderlog import Game
g = Game(sys.argv[1])
ATT = ('last-attack', 'snipe', 'attack', 'hunt', 'pack-strike', 'regicide', 'intercept', 'hunter', 'lurk')
dead_round = collections.defaultdict(list)
for_rows = []
def oe(gg, r, kind, i, d):
    if kind == 'die':
        dead_round[r].append((i, gg.side(i), d['why'], d['body'][0] if d['body'] else None, len(d['body']), i in (0, 1)))
g.run(onevent=oe)
g2 = Game(sys.argv[1]); st = {}
res = collections.Counter(); enemyLen = collections.Counter(); qkills = 0
for r, lst in sorted(dead_round.items()):
    for (i, side, why, h, L, isq) in lst:
        if side != 'us' or why != 'head-on collision': continue
        # last status of i before r
        pass
# statuses
last = {}
for r, t in g.events:
    m = re.match(r'dragon (\d+): (.*)', t)
    if m: last[int(m.group(1))] = (r, m.group(2))
    m = re.match(r'dragon (\d+)( \(queen\))? dies\. Reason: head-on collision', t)
    if m:
        i = int(m.group(1))
        if g.tm(i) != g.US: continue
        stt = last.get(i, (0, ''))[1]
        word = next((w for w in ATT if re.search(r'(?<![a-z-])' + w + r'(?![a-z-])', stt)), None)
        if not word: continue
        lst = dead_round[r]
        me = next(x for x in lst if x[0] == i)
        partner = [x for x in lst if x[1] == 'opp' and x[2] == 'head-on collision' and x[3] and me[3] and g.map.dist(x[3], me[3]) <= 1]
        if partner:
            res[(word, 'traded')] += 1; enemyLen[partner[0][4]] += 1
            qkills += any(p[5] for p in partner)
        else:
            res[(word, 'died alone')] += 1
print(g.name, 'our attack-type head-on deaths:', dict(res), '| enemy lengths taken', dict(sorted(enemyLen.items())), '| enemy queens killed', qkills)
