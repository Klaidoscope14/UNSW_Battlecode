"""local2ladder.py IN.txt OUT.txt MAPNAME : convert a tools/replay_to_txt.mjs log of a LOCAL game
(our bot = team A) into the ladder log format ladderlog.py reads (3 header lines, 'round<TAB>event',
rounds 1-based, 'dragon N: status', 'asks to split off K cells')."""
import sys, re
src, dst = sys.argv[1], sys.argv[2]
out = ['WHO STARTED MATCH : Low Cortisol']
res = ''
lines = open(src).read().split('\n')
for l in lines[:6]:
    if l.startswith('result:'):
        res = l
out.append(('REASON FOR WIN : ' if 'winner A' in res else 'REASON FOR LOSS : ') + res)
out.append('')
for l in lines:
    m = re.match(r'(\d+) (?:\[[AB]\] )?(.*)', l)
    if not m:
        continue
    r = int(m.group(1)) + 1; t = m.group(2)
    t = re.sub(r'^dragon (\d+) indicator: ', r'dragon \1: ', t)
    t = re.sub(r'asks to split off (\d+)$', r'asks to split off \1 cells', t)
    t = re.sub(r' \(parent keeps \d+\)', '', t)
    t = re.sub(r' dies: ', ' dies. Reason: ', t)
    t = t.replace('(QUEEN) dies', '(queen) dies')
    out.append(f'{r}\t{t}')
open(dst, 'w').write('\n'.join(out) + '\n')
