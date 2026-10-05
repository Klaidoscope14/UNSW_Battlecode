"""infant.py LOG : split children that die within 2 rounds of birth, by the parent's status word
when it split (from its last status text) and the child's death cause; per side."""
import sys, collections, re
from ladderlog import Game
g = Game(sys.argv[1])
born = {}
c = collections.Counter(); tot = collections.Counter()
def oe(gg, r, kind, i, d):
    if kind == 'split':
        st = gg.ind.get(i, '')
        m = re.findall(r'[a-z][a-z-]+', st)
        word = next((w for w in m if w not in ('cmb', 'col', 'exp', 'prt', 'nrm', 'war', 'keep', 'farm')), '?')
        born[d['child']] = (r, word, gg.side(i), d['k'], d['parentLen'])
        tot[(gg.side(i), word)] += 1
    elif kind == 'die' and i in born:
        r0, word, side, k, pl = born[i]
        if r - r0 <= 2:
            c[(side, word, d['why'])] += 1
g.run(onevent=oe)
print(g.name, '- children dead within 2 rounds of birth (side, parent status, cause): count / splits with that status')
for k, v in c.most_common(12):
    print(f'   {v:4d} / {tot[(k[0], k[1])]:<4d} {k}')
