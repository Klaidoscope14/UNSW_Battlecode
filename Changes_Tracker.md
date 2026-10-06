# Fleet bot — changes tracker

Updated after every step. Suite = the 204-game check (`tools/testing/suite.sh`); baseline v2.16.0 = **197-7, queen deaths 44, protected maps 84-0**.
Current reference (after B1b): `results/sB1b.txt` = **197-7, Qd 38, prot 83-1**; trace baseline `base2` (before 3 / B1b).
Acceptance rule: at most one extra loss on the protected maps, no rise in queen deaths (noise ≈ ±3 wins / ±5 queen deaths).

## Done (Architecture_v2)

| Step | What | Result |
|---|---|---|
| 0 | Decision trace (`FLEET_TRACE`, `trace.py`), unique-switch check | tool |
| 1 | `main.cpp` split into 15 layer headers + `config.hpp` | identical (204) |
| 2 | `enum Intent`, `Situation sit`, `Duty my` | identical (204) |
| 4 | `decide()` → 40 stages: reflex table, target steps, act | identical (204) |
| 5 | Move-cost terms tagged by severity band (`Cost`) | identical (204) |
| 7b | Static topology (bridges, loop regions, branches); `TOPO_TRAP`, `TOPO_LOOP` ON | 198-6, Qd 42, prot 84-0 |
| 7f | Encrypted sonar (Feistel + 12-bit code + layout check) | our play identical (24); suite **198-6, Qd 41, prot 84-0** — the 29 changed games are all vs fleet11, an old copy of our bot that could read our old messages |
| 3 | GraphView (`step_open`, named unexplored-edge rules), `BodyScope` / `WallScope` (helpers never leave a simulated body or walls behind), field cache documented, topology build on flat arrays, `TOPO_EVERY` switch (kept 1) | suite 198-6, Qd 41, prot 84-0; 203/204 identical — the 1 change exposed a body-model bug (B1 below) |
| B1b | Legal step by vision (`LEGAL_BY_VISION` ON): a visible cell with no dragon on it is always enterable — the body model can keep stale segments after a portal crossing (found by step 3) | 197-7, **Qd 38**, prot 83-1 (one Portals loss, within the rule) |
| Q1 | Their queen's death rides on every STATUS message (`EQ_STATUS_BIT` ON): 47-90 of our dragons knew, our queen never heard | 197-7, Qd 38, prot 83-1 (totals unchanged, 27 games differ); Portals: she now learns 9 rounds after and lives 243 rounds longer |
| — | `tools/make_zip.py` (multi-file or `--single`), checked by unzip + sandbox | tool |

## Full track — still to do

In proposed order. "Refactor" = must stay identical (trace); "change" = judged on the suite.

| # | Change | Type | What it does / why |
|---|---|---|---|
| Q2 | Notice their queen's death (Q2a tested, rejected) | change | In 5 of 7 games nobody of ours ever knew (deaths by wall / self out of view; our striker's own message dies with it). Ideas: a striker announces the turn before it commits; stronger 'probably dead' inference; a bunker that works above length 3 (EQ_INFER keeps her long, and a 2x2 loop needs length <= 3). |
| Q3 | Unseen long strikers | change | A length-8 enemy can sprint 8 cells: it strikes from outside her 7x7 view. Ideas: count cells beyond her view as a risk when enemies are known to be near (reports), keep a screen in the direction enemies come from. |
| Q4 | Cornered by a crowd | change | 3-5 enemy heads round her, every move inside a reach (flee picks a corner). Ideas: flee earlier (at 2 heads), toward teammates; guards intercept a crowd forming. |
| 6d | Claims for one-of-a-kind roles (giant, heir, screen per side) | change | A dragon announces the role, the longest (then lowest id) wins, the claim expires. Cures the "20-28 leaders at once" class of bug. Needs the extended message type. |
| 6e | Situation consensus | change | Majority and the enemy queen's status travel in `T_PHASE`, so the fleet switches together (today each dragon decides from its own view). |
| 7d | Queen topological leash | change | Her pearls / rest only where a loop longer than her exists and our Voronoi margin is safe, instead of a fixed radius 7. |
| 7e | Feeder delivery tiles | change | Feeders die so their pearls land inside the queen's loop region (feeding returned only 5-15% of the fed length to her). |
| 7j | Chokepoint guards | change | Guards post at the topology's chokepoints between our queen's region and the enemy side. |
| 7h | Voronoi in turns | change | Arrival time counted in turns (cells ÷ free steps): a long enemy covers several cells a turn. |
| 7a | Density prefix sums | change | O(1) richness / enemy-density boxes for zones, outposts and the queen's safest area. |
| 7c | MapProfile | change | One place for map class (size, kelp share, maze-ness from topology, portals) instead of six size thresholds. |
| 6b | Role duty + pearl leash | change | Each role's override of the pearl goal uses an explicit leash (hunt 4, feed 6, pack 2, guard 4, defender 10, zone 8). |
| 6c | Intent memory | change | Keep the current goal unless a new one is better by a margin (stops zone/pearl/guard flip-flops). |
| 6f | Quota table for share roles | change | Hunter / guard / wall shares from one table by phase, strength and map size. |
| 7g | Cork duty | change | Their queen inside a dead-end branch: one short dragon takes the branch mouth. |
| 7i | Matching experiment | change | Local auction instead of greedy pearl matching (low priority: path matching lost pearls before). |

## Diagnosis notes

**Q — our queen's head-on deaths (2026-10-07).** 20 head-on queen deaths in the suite (`QDIAG` per-turn log + replays of 9). Patterns:
(A) killer never in her view — long enemies sprint from outside the 7x7 window; (B) cornered by 3-5 heads, every move in a reach;
(C) a length-2 stalker beside her; (D) the plain sum picked a cell inside a strike reach over a corner (−2.6M safety vs −1.2M life);
(E) a length-19 queen exploring into a trap. In 4 of 9 replayed deaths their queen was ALREADY dead and ours did not know:
in 2 games 47-90 of our dragons knew within a round but the news never reached her (→ Q1); in 5 of 7 games nobody of ours
ever noticed (→ Q2). Debug: compile `QDIAG`, set `FLEET_DIAG=<file>` (and `QDIAG_ALL=1` for every turn).

## Rejected after testing

Kept in the code as switches (OFF) unless noted.

| Change (switch) | When | Result | Why rejected |
|---|---|---|---|
| Bands: life > safety > goal+style (`BAND_COMPARE 1`) | 2026-10-07 | 194-10, Qd 42, Portals 81-3 | The queen gave up room for tiny safety gains (room is a goal term) and trapped herself. |
| Bands: life first, then the sum (`BAND_COMPARE 2`) | 2026-10-07 | 194-10, Qd 48, prot 83-1 | Changed only 3 queen moves in 291k dragon-turns; the rest is noise. Corner-vs-threat is rare. |
| Queen splits to 2 and explores alone (`OPEN_QUEEN`) | v2.16.0 | 191-13 vs 197-7 | Cost 6 wins, same queen deaths. |
| Path distances in pearl matching (`MATCH_PATHS`, C) | v2.15.4 | Opening pearls by r150: Around UNSW 555 → 426, Islands 358 → 332 | Manhattan under-estimates teammates' distance, which spreads dragons; "accurate" costs crowd them. |
| Break-even dives banned (`DIVE_MATE_GAIN 1`) | v2.15.5 | UNSW opening 555 → 465, Weakhold 111 → 89 | Break-even pocket dives are the mining engine. |
| Static trap detection on Around UNSW (B) | v2.15.5 | no gain | The bot already knew those slots were traps; the dives were deliberate. |
| Centre pull stops once seen (`CENTRE_KNOWN`) | v2.15.3 | Weakhold / Australia openings worse | — |
| Outposts need spawn tiles (`OUTPOST_MIN_TILES 3`) | v2.15.3 | Weakhold opening 111 → 70 | — |
| Portal rush, 6 steps without a pearl guard | v2.15.1 | Islands opening 352 → 230 | The 4-step version with a pearl guard passed; `PORTAL_RUSH` is kept OFF (candidate to turn on). |
| Wall on small maps | v2.15.0 | Portals: queen self-traps among long wall members | Wall limited to maps ≥ 1500 cells (`WALL_MIN_CELLS`). |
| Rich-area weights for pearls / camps (`RV_PEARL_W`, `RV_CAMP_W`) | v2.14.5 | Portals 9-3 → 7-5 | Only the opening centre pull is kept. |
| Feeding fix (`FEED_FIX`, P4) | v2.14.3 | Locally Qd 33 → 39, more protected losses | Kept ON as a ladder trial (user's choice); set 0 to return to v2.14.2. |
| Adaptive queen leash + opening split (`QUEEN_ADAPT`, P2) | v2.14.1 | Islands 1 → 4-6 queen deaths / 6; protected 82-1 → 77-5 | More queen freedom = more queen deaths; leaves her at length 2 on pocket maps. |
| Queen never suicides; stall instead (`QUEEN_NO_SUICIDE`, P3b) | v2.14.2 | no fewer queen deaths | — |
| Paid steps in the two-turn lookahead (`LOOK2_PAID`) | v2.14.2 | worse than free steps only | — |
| Rich-zone weights, path feeder test, "drops are hers", earlier feeding (P4 variants) | v2.14.2 | Qd 33 → 39 | — |
| Queen evade / paid escape / stalker intercept (`QUEEN_EVADE`, `QUEEN_PAID_ESCAPE`, `STALKER_INTERCEPT`) | v2.6.0 | −7 wins vs fleet11 together | — |
| Unseen enemy length guess 6 (`UNSEEN_LEN_GUESS 6`) | v2.6.0 | −8 wins vs fleet11 | Boxed the queen in; kept at 4. |
| Loose enemy-queen death inference (`EQ_STRICT 0`) | v2.6.0 | 2 false alarms in 176 games | Strict rule: 0 false alarms. |
| Unknown edges cost 2 in routing (`UNKNOWN_EDGE_COST 2`) | v2.5.0 | about −8 wins | Kept at 1. |
| Close-ring guards | v2.4.0 | 217 wins, 155 queen deaths | Guards boxed the queen in; screen design instead. |
| Hunter share 40% / 20% (no packs) | v2.4.0 | −7 to −10 wins | Later replaced by coordinated packs. |
| Per-turn dive budget without leader dives | v2.4.0 | about −12 wins | Budget scaled by length instead. |
| Teammates give the queen way (`QUEEN_CLEAR 4`) | v2.3.0 | wall deaths 40 → 33 but head-ons 43 → 51 | Teammates near her were shielding her. |
| Teammates measure the queen's room (`QUEEN_ROOM`) | v2.2.0 | no gain | They cannot see her whole body. |
| Ignore pearls reachable only at a loss (`SKIP_PEARL_ROUNDS`) | v2.1.0 | default_small 31-9 vs 36-4 | — |
| Frontier counts as an exit (`FRONTIER_OPEN`) | v2.1.0 | no gain | — |
| Dive value replaces trap penalties | v2.1.0 | default_small 12/20 | Dive is only an extra option. |
| Survival search: unexplored edge = safe (`SURV_UNKNOWN_SAFE 1`) | v2.2.0 | long queens coiled into their own unseen body | Replaced by 0. |
| Growth splits need room (`SPLIT_ROOM`) | v2.14.4 | — | OFF for faster splitting. |
| Never-seen enemy queen counts as probably dead from r150 (`EQ_INFER_UNSEEN`, Q2a) | 2026-10-07 | 197-7, Qd 39 (Q1: 197-7, Qd 38); 82 games changed, wins and losses swapped both ways | No gain locally, and against a ladder opponent hiding a live queen ours would stop eating from r150. |
| Body model drops visibly-wrong segments (`BODY_TRUST_VISION`, B1) | 2026-10-07 | 195-9, Qd 38, Portals 81-3 | Cut the whole remembered tail after the first wrong cell, including the correct part beyond a portal: the queen forgot her own body and hit herself 3 times on Portals. Replaced by B1b (legality only). |
| Sprint escape / sprint-to-pearl / echo-only caution / doubled early density | v1.1.0 | no gain | — |
| Throttled topology rebuild (`TOPO_EVERY 4`) | 2026-10-07 | CPU p50 28.1M → 25.9M, max 48.1M → 46.0M (64×64) | Not worth a behaviour change: rebuild costs ~2.7M/turn, we have >2× headroom. Kept at 1 (exact). |

## Rejected by analysis (never built)

| Idea | Why |
|---|---|
| Adversarial A* | Speed is not the problem (BFS ≈ 0.3M of the 100M budget); its content is Voronoi (done). |
| Zobrist hashing | The loop history is 16 cells; nothing to speed up. |
| 2D Fenwick tree / quadtree | Prefix sums rebuilt per turn are simpler; no point updates needed. |
| Bitboards | Timed bodies (tails freeing over time) don't fit a bitmask; CPU is not the limit. |
| Sprint DP | Free steps ≤ 3; the sprint DFS is exact; long paid strikes use BFS. |
| Global Hungarian / min-cost-flow matching | A global optimum flips when one far input changes, and every dragon sees slightly different inputs (7i tests a local version instead). |
