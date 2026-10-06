#!/usr/bin/env python3
"""Decision-trace check for pure refactors (Architecture_v2 step 0).

  trace.py run NAME [DEF=VAL ...]   build fleet/ (with #define overrides) into work/NAME, play the trace game set
                                    with FLEET_TRACE on, keep one merged trace per game in work/trace/NAME/
  trace.py diff A B [--actions]     compare two runs game by game: IDENTICAL, or the first (round, dragon) that
                                    differs in each game, with both lines. --actions ignores the sonar values (for
                                    a change of message format: same moves, splits and notes = same play)

A trace line is: round id length head action sonars | note (one per dragon-turn, our team only). Identical traces
mean identical games: same moves, splits and sonar values every turn. ~1-2 min for the 24 games.
"""
import hashlib, os, re, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

T = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(T))
WORK = os.path.join(T, "work")
env = dict(os.environ, PATH=os.path.expanduser("~/.local/bin") + ":" + os.environ["PATH"], UNSWBC_NO_UPDATE="1")

# 8 maps x vib2 x 2 sides + 4 maps x swarm2 x 2 sides, seed 1: portals, mazes, pockets, open, small.
GAMES = [(m, "vib2", s) for m in ("portals", "maze", "weakhold", "unsw", "devil", "tower_defense", "islands", "stripes")
         for s in "AB"] + [(m, "swarm2", s) for m in ("australia", "trophy", "schooltime", "default") for s in "AB"]


def build(name, defs):
    b = os.path.join(WORK, name)
    shutil.rmtree(b, ignore_errors=True)
    os.makedirs(b)
    src = os.path.join(ROOT, "fleet")
    for f in os.listdir(src):
        if f.endswith((".hpp", ".cpp", ".h", ".toml")) and f != "main.cpp":
            shutil.copy(os.path.join(src, f), b)
    with open(os.path.join(b, "main.cpp"), "w") as out:
        for d in defs:
            k, v = d.split("=", 1)
            out.write(f"#define {k} {v}\n")
        out.write(open(os.path.join(src, "main.cpp")).read())
    # build once before the parallel games (parallel rebuilds clash)
    r = subprocess.run(["unswbc", "run", "maps/default_small.map", b, os.path.join(T, "sparring/vib2"), "--seed", "1", "--no-replay"],
                       cwd=ROOT, env=env, capture_output=True, text=True, timeout=900)
    if re.search(r"error:", r.stdout + r.stderr, re.I) or r.returncode != 0:
        sys.exit("BUILD FAILED:\n" + (r.stdout + r.stderr)[-3000:])
    return b


def play(name, b, game):
    m, opp, side = game
    gid = f"{m}_{opp}_{side}"
    raw = os.path.join(WORK, "trace", name, gid + ".d")
    shutil.rmtree(raw, ignore_errors=True)
    os.makedirs(raw)
    a, c = (b, os.path.join(T, "sparring", opp)) if side == "A" else (os.path.join(T, "sparring", opp), b)
    r = subprocess.run(["unswbc", "run", f"maps/{m}.map", a, c, "--seed", "1", "--no-replay"], cwd=ROOT,
                       env=dict(env, FLEET_TRACE=raw), capture_output=True, text=True, timeout=900)
    txt = r.stdout + r.stderr
    mm = re.search(r"team (A|B) wins after (\d+) rounds \(([^)]*)\)", txt)
    res = (("W" if mm.group(1) == side else "L") + f" ({mm.group(3)}, {mm.group(2)}r)") if mm else ("D" if re.search("draw", txt, re.I) else "?")
    lines = []
    for f in os.listdir(raw):
        if f.startswith(side):  # our team's dragons only
            lines += open(os.path.join(raw, f)).read().splitlines()
    lines.sort(key=lambda l: (int(l.split()[0]), int(l.split()[1])))
    out = os.path.join(WORK, "trace", name, gid + ".txt")
    with open(out, "w") as fo:
        fo.write(f"# result {res}\n")
        fo.write("\n".join(lines) + "\n")
    shutil.rmtree(raw, ignore_errors=True)
    return gid, res, len(lines), hashlib.md5(open(out, "rb").read()).hexdigest()[:10]


def run(name, defs):
    b = build(name, defs)
    d = os.path.join(WORK, "trace", name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    with ThreadPoolExecutor(8) as ex:
        res = list(ex.map(lambda g: play(name, b, g), GAMES))
    for gid, r, n, h in res:
        print(f"{gid:>28}  {r:<34} {n:>6} turns  {h}")
    bad = [g for g, r, n, h in res if r == "?" or n == 0]
    print(f"{len(res)} games traced -> {d}" + (f"  PROBLEM: {bad}" if bad else ""))


def diff(a, b, actions=False):
    da, db = os.path.join(WORK, "trace", a), os.path.join(WORK, "trace", b)
    strip = (lambda ls: [re.sub(r' [NESW]\d+', '', l) for l in ls]) if actions else (lambda ls: ls)
    games = sorted(f for f in os.listdir(da) if f.endswith(".txt"))
    ndiff = 0
    for g in games:
        pb = os.path.join(db, g)
        if not os.path.exists(pb):
            print(f"{g}: missing in {b}")
            ndiff += 1
            continue
        la, lb = strip(open(os.path.join(da, g)).read().splitlines()), strip(open(pb).read().splitlines())
        if la == lb:
            continue
        ndiff += 1
        ha, hb, la, lb = la[0], lb[0], la[1:], lb[1:]  # result header, then the turns
        i = next((k for k in range(min(len(la), len(lb))) if la[k] != lb[k]), min(len(la), len(lb)))
        print(f"{g}: DIFFERS at turn line {i}  ({ha[9:]} | {hb[9:]})")
        print(f"   {a}: {la[i] if i < len(la) else '<end>'}")
        print(f"   {b}: {lb[i] if i < len(lb) else '<end>'}")
    print(f"IDENTICAL ({len(games)} games)" if ndiff == 0 else f"{ndiff} of {len(games)} games differ")
    return ndiff


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "run":
        run(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "diff":
        sys.exit(1 if diff(sys.argv[2], sys.argv[3], "--actions" in sys.argv) else 0)
    else:
        sys.exit(__doc__)
