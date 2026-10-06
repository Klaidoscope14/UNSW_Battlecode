import sys,re,collections
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from ladderlog import Game
def w(t):
    for x in re.findall(r'[a-z][a-z-]+', t or ''):
        if x not in ('cmb','col','exp','prt','nrm','war','keep','farm'): return x
    return ''
g=Game(sys.argv[1]); B=int(sys.argv[2]) if len(sys.argv)>2 else 25; END=int(sys.argv[3]) if len(sys.argv)>3 else 150
st={}; mix=collections.defaultdict(collections.Counter); qmix=collections.defaultdict(collections.Counter)
def oe(gg,r,k,i,d):
    if k=='log': st[i]=d
def onr(gg,r):
    if r>END: return
    for i in g.alive:
        if g.side(i)=='us':
            (qmix if i==g.US else mix)[(r-1)//B][w(st.get(i,''))]+=1
g.run(onround=onr,onevent=oe)
for b in sorted(mix):
    tot=sum(mix[b].values())
    print(f"r{b*B}-{b*B+B-1}: "+', '.join(f"{k} {100*v/tot:.0f}%" for k,v in mix[b].most_common(7)), ' | QUEEN:', ', '.join(f"{k} {v}" for k,v in qmix[b].most_common(3)))
