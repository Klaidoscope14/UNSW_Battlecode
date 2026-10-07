# Fleet bot — handoff (state on 2026-10-06)

Read this first, then `Architecture_Plan.txt` (the agreed next job) and `Combined_Changes.txt` (the user's ideas).

## The project
- UNSW Battlecode ("Slay Queen" rules), C++ bot in `fleet/`: since Architecture_v2 step 1 the code is split into layer
  headers (`config.hpp` = all switches by module, `core/world/state/comms/perceive/graph/fields/situation/topology/
  commander/intent/tactics/act/decide/speak.hpp`), included in order by `main.cpp` (one translation unit, turn loop only).
  `helper.hpp` = engine API, `bot.toml`. The layer list is at the top of `main.cpp`.
- Step 2: team-level flags live in `Situation sit` (sit.survival, sit.switched, sit.enemyQueenDead...) and this dragon's
  roles in `Duty my` (my.leader, my.hunter, my.guard...), both in `state.hpp`; goals are `enum Intent` (I_PEARL...,
  `intent_name()` gives the old strings, so indicators and traces are unchanged).
- Step 4: `decide()` (decide.hpp) is a short pipeline: prepare_turn -> analyse_moves -> read_contact -> the REFLEX
  table (rx_*, first that acts wins) -> the TARGET steps (tg_*, in order, each may replace the goal) -> commit_target
  -> feed_sacrifice / plan_split -> score_moves -> finish_move. Values shared between stages live in `Turn tn`
  (reset each turn); each stage binds `auto& x = tn.x;` for what it reads or writes.
- Step 5: every move-scoring term in `score_moves` is tagged with a severity band via `Cost` (act.hpp):
  BAND_LIFE / BAND_SAFETY / BAND_GOAL / BAND_STYLE. `cost.total` is still the plain sum in the original order (play
  unchanged); each candidate keeps `cd.bands`. `QDEBUG` logs also print the A and B band totals.
- Step 6a (tested, OFF): `BAND_COMPARE` 1 (life > safety > goal+style) cost Portals 3 games (the queen traded room
  for tiny safety gains); 2 (life first, then the sum) changed only 3 queen moves in 291k dragon-turns (suite 194-10,
  Qd 48 = noise). Kept at 0 (v2.16 play). Debug: compile with `BAND_DIAG` and set `FLEET_DIAG=<file>` to log every
  turn where the band comparison would choose differently.
- Step 7b: static topology in `topology.hpp` (`topo_build`, rebuilt lazily when `edgeVersion` changes): bridges and
  loop regions (2-edge-connected components) of the known water, the bridge forest, and for every bridge both ways a
  `Branch` summary (cells, largest loop, spawn tiles, frontier). Queries: `topo_beyond`, `topo_trap(from, dir, len)`
  (step into a fully known branch with no loop longer than len), `topo_loop_near`. Switches `TOPO_TRAP`, `TOPO_LOOP`.
  Verified against an independent Python reference: build with `TOPO_DIAG <round>` + `FLEET_DIAG=<file>`, then
  `python3 tools/testing/topo_check.py <file>` (also prints an ASCII render: o loop, . no cycle, = bridge end, ? frontier).
  Results (204 games): TOPO_TRAP 198-6 Qd 43 prot 84-0 (wall deaths -1.2%); + TOPO_LOOP 198-6 Qd 42 prot 84-0 (one
  long queen saved, nothing else changed). Both ON. CPU on big_empty: max 48.8M (was 39.6M; topology rebuilds as edges
  are learned).
- Step 7f: sonar wire format (comms.hpp) = 52-bit block (type 4 | payload 48) under a keyed 4-round Feistel cipher
  (`MSG_KEY`, salted by team) + 12-bit check code. Accepted only if the code matches, the type is ours and every
  payload bit past the type's layout (`layout_bits`) is zero. Self-test (`MSG_SELFTEST` + `FLEET_DIAG`): 0 round-trip
  failures; ~1e-5 of random or small foreign values accepted, never a QUEEN_DEAD/SYM. Play unchanged:
  `trace.py diff ... --actions` (ignores sonar values) IDENTICAL on 24 games.
- Step 3: `step_open(c, d)` (world.hpp) names the "unexplored edge = open" rule (GraphView comment lists all three
  rules); `BodyScope` / `WallScope` (graph.hpp): a helper's simulated body / temporary walls last only for its scope,
  outside every scope the board shows our real body. fields.hpp header lists the per-turn fields and their caches.
  `TOPO_EVERY` (rebuild at most every N rounds) exists but stays 1.
  Every dragon runs its own copy of the program; they share only sonar (4 messages/turn, 64-bit each).
- Win order after r500: longer LIVING queen, then longest dragon, then total length; elimination ends it early.
  Head-ons kill both. Sprint = free steps ceil(L/4) + paid steps (1 segment each, down to length 2). Vision 7x7 (radius 3).
- Submission zip: `python3 tools/make_zip.py NAME` -> `fleet-NAME.zip` (bot.toml + main.cpp + all fleet/*.hpp, the
  layout the docs give for C++: `include = ["*.cpp", "*.hpp", ...]`, zip directory on the include path). `--single`
  pastes every header into one main.cpp (bot.toml + main.cpp + helper.hpp, like the v2.x zips). Both were checked
  the docs' way (unzip into an empty folder, --sandbox game); the script repeats that check every time.
- Team "Low Cortisol". The user tests zips on the real ladder and sends replays; local tests are a filter, not the truth.

## User's standing rules
- Never reuse or cite old-bot code (`seadragon/`, `v1bot/`, `Latest Submission/`) — sparring only.
- No map-name hardcoding; map FEATURES (size, openness, portals) are fine.
- Keep testing short (~10–15 min per change). Analysis phases: no code changes until told.
- First ideate / plan, then implement when told. Report honestly when something does not work.

## Versions
| zip | content |
|---|---|
| `fleet-v3.2.0.zip` | **latest build** (2026-10-07): restructured code + topology, encrypted sonar, B1b, Q1, Q4, 7e, 7j, 7g. Local 196-8, Qd 36, protected 84-0. See Changes_Tracker.md |
| `fleet-v2.16.0.zip` | previous submission (197-7, Qd 44, protected 84-0) |
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
- 2026-10-07 tool fixes: `bench.py` replays a game that ended in an engine error (an "Exec format error" under load once
  wiped out 136 games of a suite); `trace.py` takes `TRACE_SRC=<folder>` to build another source tree (baseline
  `work/trace/v320` = the committed v3.2.0, made from `git archive HEAD fleet`); `opening.sh` copies all `fleet/*.hpp`
  (it still copied only helper.hpp); `opening_econ.py` also prints dragons at r30 / r50.
- `check_switches.py` reports `DIAG` (core.hpp #ifdef / #else pair) - a false positive.
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
