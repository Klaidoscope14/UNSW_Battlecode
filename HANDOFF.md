# Fleet bot — handoff (state on 2026-10-06)

Read this first, then `Architecture_Plan.txt` (the agreed next job) and `Combined_Changes.txt` (the user's ideas).

## The project
- UNSW Battlecode ("Slay Queen" rules), C++ bot in `fleet/`: since Architecture_v2 step 1 the code is split into layer
  headers (`config.hpp` = all switches by module, `core/world/state/comms/perceive/graph/fields/situation/topology/
  commander/intent/tactics/act/decide/speak.hpp`), included in order by `main.cpp` (one translation unit, turn loop only).
  `helper.hpp` = engine API, `bot.toml`. The layer list is at the top of `main.cpp`.
  Every dragon runs its own copy of the program; they share only sonar (4 messages/turn, 64-bit each).
- Win order after r500: longer LIVING queen, then longest dragon, then total length; elimination ends it early.
  Head-ons kill both. Sprint = free steps ceil(L/4) + paid steps (1 segment each, down to length 2). Vision 7x7 (radius 3).
- Submission = zip of `bot.toml`, `main.cpp` and ALL `*.hpp` in `fleet/` (older zips in the repo root had 3 files).
- Team "Low Cortisol". The user tests zips on the real ladder and sends replays; local tests are a filter, not the truth.

## User's standing rules
- Never reuse or cite old-bot code (`seadragon/`, `v1bot/`, `Latest Submission/`) — sparring only.
- No map-name hardcoding; map FEATURES (size, openness, portals) are fine.
- Keep testing short (~10–15 min per change). Analysis phases: no code changes until told.
- First ideate / plan, then implement when told. Report honestly when something does not work.

## Versions
| zip | content |
|---|---|
| `fleet-v2.16.0.zip` | **current submission**; `fleet/` = the same play, restructured (step 1: 204 games identical) |
| `fleet-v2.15.5.zip` | fallback (before the opening plan) |
| 2.15.3 / 2.15.4 | older steps (F6/F7, Voronoi) |

What each version added is in the memory file `project-fleet-bot.md` and in the comments next to each `#define`.

## Switches that matter (`fleet/config.hpp`)
ON: QUEEN_HOME, PURSUIT_SAFE, FOUNTAINS + DEADEND_STRICT (P1), THREAT_TAIL_FREE + QUEEN_LOOK2 (P3, LOOK2_PAID 0),
GIANT_ALL + GIANT_ONE (one giant/heir, leader election from length 6), SPLIT_MORE, SPLIT_ROOM 0, QUEEN_MIN_KEEP 3,
QUEEN_FIRST_SPLIT, REGION_VALUE (centre explore only), OUTPOSTS, WALL (maps >= 1500 cells), EQ_INFER (their queen
unseen 100 rounds -> ours bunkers, no trim), CHASE_MAX/DRY_MAX (pearl stall breakers), QUEEN_CROWD (flee 3+ heads),
SLOT_CHECK, CAMP_MAX_WAIT/CAMP_GIVEUP, VORONOI, QUEEN_LEN_MEM 100, OPEN_PLAN (no hunters/wall/runners until majority:
>= 8 dragons and >= 1.4x their estimate), ZONES (rich-zone allocation + T_ZONE sonar), FEED_FIX (P4, ladder trial).

OFF (tested, kept as switches): QUEEN_ADAPT (P2: more queen deaths), QUEEN_NO_SUICIDE, RV_PEARL_W/RV_CAMP_W (hurt
Portals), CENTRE_KNOWN/OUTPOST_MIN_TILES (hurt Weakhold/Australia), MATCH_PATHS (C: path distances in the pearl
matching lost pearls — greedy+manhattan spreads dragons by accident), PORTAL_RUSH (P9, passed tests, off to keep
v2.15.x comparable — candidate to turn on), OPEN_QUEEN (queen splits to 2 + explores alone: cost 6 wins).

## Testing (all in `tools/testing/`, paths relative to the repo; needs `unswbc` in ~/.local/bin and node)
- `bash tools/testing/quick.sh` — 2-min smoke test + CPU (max points/turn; v2.16.0 peaks ~40M, we shipped up to 48.6M).
- `bash tools/testing/suite.sh NAME [DEF=VAL ...]` — THE check: 204 games (17 maps x 3 sparring bots x 2 sides x
  2 seeds), ~6 min, writes `results/NAME.txt`. `SEED0=3` for seeds 3–4.
- `python3 tools/testing/compare.py v2.16.0 NAME` — totals, queen deaths, protected maps, losses per map.
- `python3 tools/testing/same.py v2.16.0 NAME` — identical-game check for pure refactors (verified: re-running
  v2.16.0 reproduces all 204 games exactly).
- `python3 tools/testing/trace.py run NAME [DEF=VAL ...]` then `trace.py diff base NAME` — THE check for pure
  refactors (Architecture_v2 steps 1-5): 24 games (~1 min) with `FLEET_TRACE` on; every dragon-turn's action, sonar
  values and note are compared, and the first diverging (round, dragon) is printed. Baseline `base` = v2.16.0
  (+ trace hooks; its 24 results match results/v2.16.0.txt). Traces live in `tools/testing/work/trace/` (~50 MB each).
- `python3 tools/testing/check_switches.py` — fails if any `#define` name is used twice.
- `bash tools/testing/opening.sh NAME "DEF=VAL" "maps"` — opening economy medians (noisy: ±150 pearls per game on
  Around UNSW; only use for big effects).
- Baselines: `results/v2.16.0.txt` (197-7, queen deaths 44, protected 84-0), `results/v2.15.5.txt` (193-11, 33, 83-1).
- Acceptance rule: at most one extra loss on the protected maps (Maze, Slithery Fight, Default, Australia, Portals,
  Around UNSW, Autarky) and no rise in queen deaths. Noise ≈ ±3 wins / ±5 queen deaths per 204 games.
- Replay analysis pipeline: `tools/replay_analysis/` (README there): `run_all.sh`, `tactics.sh`, `opening_econ.py`,
  `track.py`, `snap.py`, `status_mix.py`, `local2ladder.py`; local replays -> `node tools/replay_to_txt.mjs`.
- Debug: compile with `#define QDEBUG 1` (queen move scores) or `FLEET_DEBUG` -> LOG lines in replays.

## Gotchas that cost time
- The shell is zsh: unquoted `$var` does not word-split (use bash scripts or `${=var}`); `rm` globs that match nothing
  abort the command; guard deletions as `"${P:?}"/...`.
- Disk filled up once (replays are 5–20 MB each): delete replays after converting; `tools/testing/work/` is gitignored.
- Running bench on `fleet/` without building first -> parallel builds clash -> `?` errors (quick.sh/suite.sh build first).
- Two `#define`s with the same name silently shadow each other (`MAJ_MIN_UNITS` 12 vs 8) — use unique names.
- Opening medians are noisy; decide with the 204-game suite.

## Data on the ladder (where the losses come from)
- Analyses: memory files `ladder-v214-weakmaps.md`, `ladder-v2150-buyhigh.md`. Replays were deleted from the working
  tree but are in git: `git show HEAD:Buy-high-sell-low_Replays/<Map>.txt`; older analyses in commit `be8f998`
  (`Replays/analysis/00_COMBINED_FINDINGS.txt`).
- Main causes: our queen dies (head-ons, swarms, unseen long enemies); opening economy behind swarms; their queen dead
  but ours dies later; break-even pocket dives are PROFITABLE (do not ban them); boxed deaths in open water are dynamic.

## Next job
`Architecture_Plan.txt`: restructure into layers (perception, map model, situation/phase, team plan, behaviours +
arbiter, motion with banded cost model, comms), steps 1–5 as pure refactors verified with `same.py`, then new pieces.
The user must approve the plan and its section-6 decisions before integration starts.
