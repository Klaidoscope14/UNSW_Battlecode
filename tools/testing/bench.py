#!/usr/bin/env python3
"""Play bot vs opponents on every map from both sides; report wins and how games ended."""
import argparse, glob, os, re, subprocess, collections
from concurrent.futures import ThreadPoolExecutor
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
env = dict(os.environ, PATH=os.path.expanduser("~/.local/bin") + ":" + os.environ["PATH"], UNSWBC_NO_UPDATE="1")

def play(mapf, a, b, seed, sandbox):
    cmd = ["unswbc", "run", mapf, a, b, "--seed", str(seed), "--no-replay"] + (["--sandbox"] if sandbox else [])
    out = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, timeout=900)
    txt = out.stdout + out.stderr
    m = re.search(r"team (A|B) wins after (\d+) rounds \(([^)]*)\)", txt)
    if m: return m.group(1), m.group(3), int(m.group(2)), txt
    if re.search(r"draw", txt, re.I): return "D", "draw", 500, txt
    return "?", "error", 0, txt

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bot", default="fleet"); ap.add_argument("--opps", default="")
    ap.add_argument("--maps", default=""); ap.add_argument("--seeds", type=int, default=1); ap.add_argument("--seed0", type=int, default=1)
    ap.add_argument("--sandbox", action="store_true"); ap.add_argument("-j", type=int, default=6)
    a = ap.parse_args()
    maps = sorted(glob.glob(os.path.join(ROOT, "maps/*.map")))
    if a.maps: maps = [m for m in maps if os.path.basename(m)[:-4] in a.maps.split(",")]
    jobs = []
    for opp in a.opps.split(","):
        for m in maps:
            for s in range(a.seed0, a.seed0 + a.seeds):
                jobs.append((opp, m, "A", s)); jobs.append((opp, m, "B", s))
    def run(j):
        opp, m, side, s = j
        x, y = (a.bot, opp) if side == "A" else (opp, a.bot)
        w, how, rounds, txt = play(m, x, y, s, a.sandbox)
        res = "D" if w == "D" else ("?" if w == "?" else ("W" if w == side else "L"))
        deaths = collections.Counter()
        ourTeam = side
        ourQ = "0" if side == "A" else "1"
        qm = re.search(r"round (\d+): bot %s \(team %s\) died: (.*)" % (ourQ, side), txt)
        qdead = (qm.group(2).strip() + " r" + qm.group(1)) if qm else ""
        eq = "1" if side == "A" else "0"; eside = "B" if side == "A" else "A"
        edead = re.search(r"bot %s \(team %s\) died" % (eq, eside), txt) is not None
        for line in txt.splitlines():
            mm = re.search(r"\(team (A|B)\) died: (.*)", line)
            if mm and mm.group(1) == ourTeam: deaths[mm.group(2).strip()] += 1
        return opp, os.path.basename(m)[:-4], side, s, res, how, rounds, deaths, qdead, edead
    with ThreadPoolExecutor(a.j) as ex:
        results = list(ex.map(run, jobs))
    tot = collections.Counter(); deaths = collections.Counter(); per = collections.defaultdict(collections.Counter)
    ed = 0; qd = 0; qcause = collections.Counter()
    for opp, m, side, s, res, how, rounds, d, q, eqd in results:
        ed += 1 if eqd else 0; qd += 1 if q else 0; qcause[q.split(' r')[0]] += 1 if q else 0
        print(f"{opp:>18} {m:>16} {side} s{s}: {res} ({how}, {rounds}r){(' Qdead:' + q) if q else ''}")
        tot[res] += 1; deaths.update(d); per[opp][res] += 1
    for opp, c in per.items(): print(f"vs {opp}: W{c['W']} L{c['L']} D{c['D']} ?{c['?']}")
    print(f"queen deaths {qd}/{len(results)} enemy queen deaths {ed} {dict(qcause)}"); print(f"TOTAL W{tot['W']} L{tot['L']} D{tot['D']} ?{tot['?']}  our deaths: {dict(deaths.most_common())}")
main()
