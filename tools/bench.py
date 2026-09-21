#!/usr/bin/env python3
"""Benchmark harness for the Sea Dragons bot (proposal #30 telemetry).

Runs a bot across every map, both sides, against one or more opponents, and
aggregates:
  * win / loss / draw and how games end (elimination vs length vs draw)
  * our team's death-reason breakdown (self-coil, wall, collision, head-to-head)
  * per-dragon length / pearls-eaten / reachable-area, from bot-side telemetry
    (a SEAD_TELEMETRY build; the shipped bot carries none of it)

Because the engine is deterministic per (map, botA, botB), the "sample" is the
set of map x side x opponent games -- richer coverage, not repeated runs. This
harness is a dev tool; only the seadragon/ project is ever submitted.

Usage:
  python3 tools/bench.py --bot seadragon --opps v1bot,starter
  python3 tools/bench.py --bot seadragon --baseline v2base --opps v1bot
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UNSWBC = os.environ.get("UNSWBC", str(Path.home() / ".local/bin/unswbc"))

RESULT_WIN = re.compile(r"team ([AB]) wins after (\d+) rounds \(([^)]+)\)")
RESULT_DRAW = re.compile(r"draw after (\d+) rounds")
DEATH = re.compile(r"round (\d+): bot (\d+) \(team ([AB])\) died: (.+)")

REASON = {  # engine phrase -> bucket
    "hit itself": "self",
    "hit a wall": "wall",
    "hit another dragon": "collide",
    "lost a head-to-head": "head2head",
}


def bucket(phrase):
    for k, v in REASON.items():
        if k in phrase:
            return v
    return "fault"  # no valid action / timeout / crash


def make_tel_build(bot):
    """Return a temp bot dir identical to `bot` but compiled with telemetry."""
    src = ROOT / bot
    dst = Path(tempfile.mkdtemp(prefix=f"tel_{bot}_")) / bot
    shutil.copytree(src, dst)
    main = dst / "main.cpp"
    if main.exists():
        main.write_text("#define SEAD_TELEMETRY 1\n" + main.read_text())
    return dst


def maps_available():
    return sorted(p.stem for p in (ROOT / "maps").glob("*.map"))


def run_game(map_name, a_dir, b_dir, tel_dir):
    """Run one game; return (stdout, telemetry rows). Each dragon wrote one file."""
    env = dict(os.environ)
    env["SEAD_TEL_DIR"] = str(tel_dir)
    env["PATH"] = str(Path(UNSWBC).parent) + os.pathsep + env.get("PATH", "")
    if tel_dir.exists():
        shutil.rmtree(tel_dir)
    tel_dir.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(
        [UNSWBC, "run", str(ROOT / "maps" / f"{map_name}.map"), str(a_dir), str(b_dir)],
        capture_output=True, text=True, env=env, cwd=ROOT,
    )
    out = (r.stdout or "") + "\n" + (r.stderr or "")  # deaths/progress land on stderr
    rows = []
    for csv in tel_dir.glob("*.csv"):
        f = csv.read_text().strip().split(",")
        if len(f) == 10:
            rows.append(dict(team=f[0], id=int(f[1]), first=int(f[2]),
                             last=int(f[3]), maxLen=int(f[4]), finalLen=int(f[5]),
                             ate=int(f[6]), splits=int(f[7]),
                             areaSum=int(f[8]), turns=int(f[9])))
    return out, rows


def parse_result(out, our_team):
    m = RESULT_WIN.search(out)
    if m:
        winner, rounds, how = m.group(1), int(m.group(2)), m.group(3)
        return ("W" if winner == our_team else "L"), rounds, how
    m = RESULT_DRAW.search(out)
    if m:
        return "D", int(m.group(1)), "draw"
    return "?", 0, "?"


def evaluate(bot, opps, map_list):
    tel_build = make_tel_build(bot)
    tel_bot = tel_build  # path to the telemetry build's project dir
    tel_dir = Path(tempfile.gettempdir()) / f"seadtel_{os.getpid()}"

    agg = dict(games=0, W=0, L=0, D=0, rounds=0,
               ends={}, deaths={}, our_deaths=0,
               max_len=0, final_len_sum=0, ate_sum=0, splits_sum=0,
               dragons=0, area_num=0, area_den=0, lifespan_sum=0)

    for opp in opps:
        opp_dir = ROOT / opp
        for m in map_list:
            for side in ("A", "B"):
                a, b = (tel_bot, opp_dir) if side == "A" else (opp_dir, tel_bot)
                out, rows = run_game(m, a, b, tel_dir)
                res, rounds, how = parse_result(out, side)
                agg["games"] += 1
                agg[res] = agg.get(res, 0) + 1
                agg["rounds"] += rounds
                agg["ends"][how] = agg["ends"].get(how, 0) + 1
                for dm in DEATH.finditer(out):
                    if dm.group(3) == side:  # our team's dragon died
                        b_ = bucket(dm.group(4))
                        agg["deaths"][b_] = agg["deaths"].get(b_, 0) + 1
                        agg["our_deaths"] += 1
                # our telemetry rows are the ones on our side
                ours = [r for r in rows if r["team"] == side]
                for r in ours:
                    agg["dragons"] += 1
                    agg["max_len"] = max(agg["max_len"], r["maxLen"])
                    agg["final_len_sum"] += r["finalLen"]
                    agg["ate_sum"] += r["ate"]
                    agg["splits_sum"] += r["splits"]
                    agg["area_num"] += r["areaSum"]
                    agg["area_den"] += r["turns"]
                    agg["lifespan_sum"] += r["last"] - r["first"] + 1
    shutil.rmtree(tel_build.parent, ignore_errors=True); shutil.rmtree(tel_dir, ignore_errors=True)
    return agg


def summarize(name, a):
    g = max(1, a["games"])
    d = max(1, a["dragons"])
    print(f"\n=== {name} ===")
    print(f"  record        {a['W']}-{a['L']}-{a['D']}  ({100*a['W']//g}% win)  over {a['games']} games")
    print(f"  avg game len  {a['rounds']//g} rounds")
    print(f"  game endings  " + ", ".join(f"{k}:{v}" for k, v in sorted(a["ends"].items())))
    print(f"  our deaths    {a['our_deaths']} total -> " +
          (", ".join(f"{k}:{v}" for k, v in sorted(a["deaths"].items())) or "none"))
    print(f"  dragons       {a['dragons']}  avg lifespan {a['lifespan_sum']//d} rounds  splits {a['splits_sum']}")
    print(f"  length        max {a['max_len']}  avg final {a['final_len_sum']//d}  pearls eaten/dragon {a['ate_sum']//d}")
    if a["area_den"]:
        print(f"  avg reach area {a['area_num']//a['area_den']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bot", default="seadragon")
    ap.add_argument("--baseline", default=None, help="second bot to compare against")
    ap.add_argument("--opps", default="v1bot,starter")
    ap.add_argument("--maps", default="all", help="'all' or comma list of map stems")
    args = ap.parse_args()

    opps = [o for o in args.opps.split(",") if o]
    map_list = maps_available() if args.maps == "all" else args.maps.split(",")
    print(f"maps: {', '.join(map_list)}")
    print(f"opponents: {', '.join(opps)}  ({len(map_list)*len(opps)*2} games per bot)")

    summarize(args.bot, evaluate(args.bot, opps, map_list))
    if args.baseline:
        summarize(args.baseline, evaluate(args.baseline, opps, map_list))


if __name__ == "__main__":
    main()
