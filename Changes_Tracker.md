# Fleet bot — changes tracker

Updated after every step. Suite = the 204-game check (`tools/testing/suite.sh`); baseline v2.16.0 = **197-7, queen deaths 44, protected maps 84-0**.
Current reference = **v3.2.0** (`fleet-v3.2.0.zip`, `results/sV320.txt`): **196-8, Qd 36, protected 84-0**. ON since v2.16.0: topology (7b), encrypted sonar (7f), B1b, Q1, Q4, 7e, 7j, 7g. CPU max 49.7M on 64x64.
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
| 7c / 6b / 6f | Refactors: MapProfile (map class in one struct), pearl-leash table (`kLeash`), slot plan (`kSlots`) | identical (204) |
| 7e | Feeder delivery (`FEED_DELIVERY` ON): a feeder dies only when 2/3 of its pearls land within 5 water steps of the queen's head | 198-6, Qd 38, **prot 84-0** |
| 7j | Chokepoint guards (`CHOKE_GUARDS` ON) — kept by the user's call; first version never found a post (path search ignored unexplored water), fixed | in v3.2.0: 196-8, Qd 36, prot 84-0 |
| 7g | Cork (`CORK` ON): locally identical — their queen is rarely seen; kept for the ladder | 196-8, Qd 36 (no game changed) |
| Q4 | The queen flees a crowd from 2 enemy heads within 6 (`QUEEN_CROWD` 3 → 2): she was cornered by 3-5 heads | 196-8, **Qd 36** (head-on 19 → 17), prot 83-1 |
| — | `tools/make_zip.py` (multi-file or `--single`), checked by unzip + sandbox | tool |
| v3.3.0 | Contradiction fixes (Battlecode_contradictions_final.md): A1 `ROLE_ORDER_FIX`, A3/A4/A7 `INTENT_GUARDS`, A6 `REACH_EST`, plus stage 1 clean-up (identical) | one suite (user: ship fast): 196-8, Qd 40, prot 84-0 |

## Full track — still to do

In proposed order. "Refactor" = must stay identical (trace); "change" = judged on the suite.

| # | Change | Type | What it does / why |
|---|---|---|---|
| Q2 | Notice their queen's death (Q2a, Q2b, Q2c tested, rejected) | change | In 5 of 7 games nobody of ours ever knew (deaths by wall / self out of view; our striker's own message dies with it). Ideas: a striker announces the turn before it commits; stronger 'probably dead' inference; a bunker that works above length 3 (EQ_INFER keeps her long, and a 2x2 loop needs length <= 3). |
| Q3 | Unseen long strikers (`REP_MAX_AGE 6` tested, rejected) | change | A length-8 enemy can sprint 8 cells: it strikes from outside her 7x7 view. Older reports in her threat map made her give up room on Portals (5 losses). Open. |

## Diagnosis notes

**Opening (2026-10-07, user: not aggressive enough).** Against the sparring bots we are level at r30 and ahead from r100 on every map;
the only gap is Around UNSW / Islands at r50 (UNSW pearls 80 vs 89, dragons 26.5 vs 29.5). The opponent's heads sit within 1 of a spawn
tile 60% of turns (ours 38%) - but making ours hold tiles (O3) cut UNSW pearls by a third. O1 (queen splits), O2 (border push) and O3 did
not close the gap; the ladder gap needs ladder replays to find.


**Q — our queen's head-on deaths (2026-10-07).** 20 head-on queen deaths in the suite (`QDIAG` per-turn log + replays of 9). Patterns:
(A) killer never in her view — long enemies sprint from outside the 7x7 window; (B) cornered by 3-5 heads, every move in a reach;
(C) a length-2 stalker beside her; (D) the plain sum picked a cell inside a strike reach over a corner (−2.6M safety vs −1.2M life);
(E) a length-19 queen exploring into a trap. In 4 of 9 replayed deaths their queen was ALREADY dead and ours did not know:
in 2 games 47-90 of our dragons knew within a round but the news never reached her (→ Q1); in 5 of 7 games nobody of ours
ever noticed (→ Q2). Debug: compile `QDIAG`, set `FLEET_DIAG=<file>` (and `QDIAG_ALL=1` for every turn).

## Designs for the overnight run (Q2 → 7i)

Each item: implemented behind a switch, one 204-game suite against the current reference, kept only if it passes the
acceptance rule. Pure refactors are checked with the trace instead (must be identical).

| # | Design | How it is judged |
|---|---|---|
| Q3 | Enemy reports stay in the queen's threat map 6 rounds instead of 3 (`REP_MAX_AGE`); each report's reach grows with its age, so she keeps room from long enemies that just left everyone's view. | suite |
| Q2 | (a) rejected. (b) Looser death inference (`EQ_STRICT 0`): a fresh death drop next to where any teammate last reported their queen counts. Was 2 false alarms in 176 games when tested alone (v2.6); now Q1 spreads the news fleet-wide, so a true detection is worth much more. (c) If (b) fails: a striker that is about to hit their queen tells teammates on the turn it *sees* her within reach (it strikes next turn only if she is still in reach). | suite |
| 6e | Fleet-wide switch: the numbers → length switch rides on every STATUS (the last free bit), like Q1. Today it travels in T_PHASE only, so parts of the fleet switch late. | suite |
| 7d | Queen topological leash: pearls up to 2 × the leash away are allowed when the pearl's loop region holds a loop longer than her (topology) and we reach it first by ≥ 2 (Voronoi); inside the leash nothing changes. A longer, still safe queen. | suite |
| 7e | Feeder delivery: a feeder sacrifices only when at least 2/3 of its drop cells (head and every 2nd segment) are within 5 path steps of the queen's head, so the pearls land where she can eat them. | suite |
| 7j | Chokepoint guards: an idle guard posts on the first bridge (topology) on the queen's path toward the enemy side, within 10 steps of her, instead of the plain guard ring. | suite |
| 7h | Voronoi in turns: arrival time = cells ÷ free steps per turn (speed classes 1 / 2 / 3+), for ours and theirs. | suite + CPU |
| 7a | Density by 2D prefix sums (wrapping box queries) for spawn-tile counts (zones, outposts, lane splits), replacing the radius-4 diamond loops. | suite + CPU |
| 7c | MapProfile: one struct for the map class (size, kelp share, maze-ness from topology, portals); the six size thresholds read it. Same values. | trace (identical) |
| 6b | Pearl leash per role as one table (hunt 4, majority hunt 2, feed 6, pack / wall 2, guard 4, defender 10, zone 8, scout +4). Same values. | trace (identical) |
| 6f | Hunter / guard / screen / defender / wall slot shares from one quota table. Same values. | trace (identical) |
| 6c | Intent memory: a non-pearl goal (zone, outpost, camp, explore, wall) is kept unless the new one is closer by 4+ steps and 30% (pearls already have this). | suite |
| 7g | Cork: a dragon that sees their queen whose head is inside a known dead-end branch (topology) goes for the branch's mouth instead of chasing her. | suite |
| 7i | Local-auction pearl matching: after the greedy matching, swap two nearby agents' pearls when that lowers the sum of their costs. | suite |

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
| Giants / elected leaders only after the switch (`ROLES_AFTER_SWITCH`, 6d') | 2026-10-07 | 196-8, Qd 40, Portals 81-3 (elimination wins 83 → 86) | More splitting grows the fleet but cost queen safety on Portals; fails the protected-map rule. The leader surplus stays a known issue. |
| Role claim messages (`ROLE_CLAIMS`, 6d) | 2026-10-07 | still up to 16 giants / ~11 leaders at once (Around UNSW, Australia) | Claims did not spread: most giants heard 0 claims in 100 rounds — sonar stops at kelp and at the first dragon. Lesson: fleet-wide agreement only works for bits riding on every STATUS (as Q1), not for dedicated messages. |
| Giant claims gossiped on every STATUS (`GIANT_GOSSIP`, 6d'') | 2026-10-07 | claim life 7 rounds: 196-8, Qd 37, prot 80-4 (Portals 4); giants at once 5.4 vs 6.2 (36 games). Claim life 28 rounds: still 8-14 giants per game (8 games) | STATUS repacked (id 13, length 7, round mod 16) to carry the best giant claim known (length, id hash, age). It spreads ~1 hop per round, and on kelp-heavy maps the sonar graph is in pieces: far giants never hear the longest one (the length-20 giant heard no claim at all). An expiring fleet-wide fact cannot form; only a permanent one (Q1) spreads. Kept as a switch, OFF. |
| Giant only from length 10 (`GIANT_MIN_LEN` 5 → 10) | 2026-10-07 | 194-10, Qd 43, prot 79-5 (Portals 5) | Most surplus giants are 5-9 long; raising the bar cost Portals games and queen deaths. |
| Queen splits at length 4 in the opening (`OPEN_QUEEN_SPLIT`, O1, user: multiply fast) | 2026-10-07 | 194-10, **Qd 40**, prot 83-1; opening unchanged on 6 of 8 maps | Her other split checks (no head within 3, two real exits, room) rarely pass early, so few extra splits; the ones that happen cost queen deaths. |
| Hold spawn tiles in the opening (`OPEN_HOLD`, O3; ladder #1 sits by spawn tiles 60% of turns vs our 38%) | 2026-10-07 | 193-11, Qd 38, prot 80-4; **Around UNSW pearls r100 293 → 199** | A tile within 3 that spawns within 12 rounds became a job, so dragons waited instead of chasing: far worse on big maps. Sitting by tiles is not what makes the opponent faster. |
| Opening border push (`OPEN_PUSH`, O2, user: occupy space) | 2026-10-07 | seeds 1-2: 194-10, Qd 35, prot 84-0 (v3.2.0 196-8, 36); **seeds 3-4: 191-13, Qd 36 (v3.2.0 191-13, Qd 32)** | Before r100 idle explorers are not kept out of their side and the border band pulls twice as hard. Neutral over 408 games (Australia pearls r100 +6); kept as a switch for a ladder trial. Also seen: v3.2.0 itself loses 6 Portals games on seeds 3-4 - its 84-0 on seeds 1-2 is partly luck. |
| Queen splits at the start (`QUEEN_START_SPLIT`, user) | 2026-10-07 | seeds 1-2: 193-11, Qd 38, prot 82-2; **seeds 3-4: 189-15, Qd 44** (v3.2.0: 196-8 Qd 36 / 191-13 Qd 32) | Before r30 the queen splits 4 → 2 + 2; only enemy heads within 3 block it (our own start crowd blocked every queen split before - she sat at length 4 to r150 on Australia). Fires at r0-7 on most maps. Opening better on Around UNSW (r50 pearls 80 → 87.5) and Islands (r50 dragons 18 → 24), worse on Australia (r150 dragons 45.5 → 33). Over 408 games: -7 wins, **+14 queen deaths**. OFF; the user's call for a ladder trial. |
| Enemy reports stay 6 rounds in the queen's threat map (`REP_MAX_AGE 6`, Q3) | 2026-10-07 | 192-12, Qd 35, prot 78-6 (Portals 5 losses) | Like 6a mode 1: a more cautious queen gives up room on Portals and traps herself. |
| Looser enemy-queen death inference (`EQ_STRICT 0`, Q2b) | 2026-10-07 | 196-8, Qd 38 (reference 196-8, Qd 36) | No gain; slightly more queen deaths. |
| Strike news (`STRIKE_NEWS`, Q2c) | 2026-10-07 | measured in log-only mode over 204 games: 2186 detections, **56 right (2.6%)**, false alarms in 75 games | A striker within next-turn reach of their queen says so every turn and "off" when she gets away (a striker dies in its strike and casts no sonar - the old same-turn QUEEN_DEAD in rx_regicide_paid is never sent). Silence for 6 rounds = dead. Sonar relays lose too many messages: silence means nothing. Never enabled. Lesson: infer only from positive evidence, never from a missing message. Also measured: in 186 games where their queen died our fleet learned it in only 52. |
| Density by prefix-sum boxes (`DENSITY_BOX`, 7a) | 2026-10-07 | alone 195-9, Portals -3; with 7e+7j+7g 194-10, prot 80-4 | The box shape moves zone / outpost choices; the only gain was CPU we do not need. Dropped for v3.2.0 (user), may return later. |
| Switch bit on every STATUS (`SWITCH_STATUS_BIT`, 6e) | 2026-10-07 | 196-8, **Qd 45**, prot 82-2 | The slow spread of the switch kept parts of the fleet splitting; switching together left fewer, longer dragons around the queen. |
| Queen topological leash (`QUEEN_TOPO_LEASH`, 7d) | 2026-10-07 | 192-12, Qd 37, prot 81-3 | Trips beyond the home leash meet more enemies; topology guards against traps, not against chasers. |
| Voronoi in turns (`VOR_TURNS`, 7h) | 2026-10-07 | 190-14, Qd 36, prot 79-5 | Long enemies look much faster: our dragons gave up contested pearls / spawn tiles (rules were tuned on cells). |
| Intent memory, always (`INTENT_MEMORY 1`, 6c) | 2026-10-07 | 193-11, **Qd 33**, prot 81-3 | Fewest queen deaths so far, but dragons kept walking to old goals when enemies came. |
| Intent memory only with no enemy in view (`INTENT_MEMORY 2`, 6c v2, user's idea) | 2026-10-07 | first run 186-18 (on the 7a combined build); **re-run on v3.2.0: 195-9, Qd 35, prot 81-3** (Portals 3) | The 186-18 came from the base it was tested on (7a), not from v2. On the real base it is neutral overall but loses 3 Portals games: fails the protected-map rule. |
| Intent memory as a chooser bonus (`INTENT_MEMORY 3`, 6c v3) | 2026-10-07 | 194-10, Qd 38, prot 81-3 (Portals 3) | Fixes what v1/v2 got wrong (the remembered goal overrode skip lists, "teammate already here" and zone reallocation): zone / outpost / camp choosers give last round's goal max(4, 30%) steps head start. Still 3 Portals losses - every sticky-goal version loses there; the fleet must re-target freely. 6c closed. |
| Pearl swap between nearby teammates (`MATCH_SWAP`, 7i) | 2026-10-07 | 189-15, Qd 40, prot 78-6 | Like MATCH_PATHS: "better" assignments crowd dragons together. |
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
