# Viability analysis of RECOMMENDED_CHANGES.txt

Date: 2026-10-07. Analysed against the code as of **v3.3.0** (`fleet/`), our test history (`Changes_Tracker.md`) and
the diagnostic runs. No code was changed for this analysis.

Verdict scale:
- **Done**: already in v3.3.0.
- **Viable**: worth building as written.
- **Viable, modified**: worth building, but only in a changed form.
- **Doubtful**: little expected gain or high risk.
- **Not viable**: conflicts with game constraints or measured evidence.

Facts used throughout (measured):
- CPU: the bot peaks at 44.6M of 100M points per turn on 64×64; the contact search costs at most ~20,500 BFS cells
  (≈1M points). **CPU is not the bottleneck.**
- Sonar: only facts that never expire spread fleet-wide (Q1). Changing facts don't converge: ROLE_CLAIMS showed giants
  hearing 0 claims in 100 rounds; GIANT_GOSSIP still gave 8–14 giants. **Each dragon's picture of the fleet is
  different.**
- Assignment: "better" assignments crowd dragons together (MATCH_PATHS: Around UNSW pearls 555 → 426; MATCH_SWAP
  189-15).
- Queen caution: more caution makes her give up room and lose on Portals (Q3: 5 losses; BAND_COMPARE 1). More freedom
  makes her die more (QUEEN_ADAPT, 7d, QUEEN_START_SPLIT).
- Goal stickiness: every sticky-goal version (INTENT_MEMORY 1/2/3) lost about 3 Portals games.
- Their queen is seen often in local games: 13,986 dragon-turns in 204 games; in a corridor 758 times.

---

## Part I — Do not re-enable MATCH_PATHS / MATCH_SWAP / VOR_TURNS / ROLE_CLAIMS
**Agree.** This matches the measured results. CONTACT_SEARCH stays OFF until its own benchmark (its A8 fix is still open).

## Parts II–IV — Existing-code fixes

| # | Item | Status | Verdict | Notes |
|---|---|---|---|---|
| A1 | ROLE_ORDER_FIX | **Done (ON in v3.3.0)** | Viable | Your document says keep it OFF until measured. v3.3.0 ships it ON because you asked to ship fast. One suite: 196-8, protected 84-0, queen deaths 40 (+4, inside noise). If you want to follow the document strictly, set it to 0 and rebuild: 1 line, play identical to v3.2.0 for that part. Small correction: `amQueen` is refreshed in `compute_fleet_state` (and at start-up), not during perception; your conclusion (not a real problem) still holds. |
| A2 | Consolidate override | **Done** (stage 1, identical trace) | Viable | — |
| A3 | POST_HOLD | **Partly done** (`INTENT_GUARDS`: explore no longer replaces I_POST) | Viable, modified | A guard of length ≥ 4 can't circle a 2×2 post, so "keep holding the post loop" isn't physically possible for it. What is done: it keeps no target and stays put instead of walking off. A real hold for long guards would need a different post shape; not worth it (the case is rare). |
| A4 | CORK_KEEP | **Done** (pack and feed keep I_CORK) | Viable, but **the diagnosis is wrong** | The diagnostic logs "cork-target" inside `tg_cork` before any later step runs, and it fired 0 times. So the cork is not lost downstream: it **never triggers**. The blocker is its rule: it needs exactly one way out of her position that is not a dead end too small for her, and unexplored water counts as a way out. With partial vision both ends of a corridor usually look open. CORK_KEEP is right but changes nothing until the trigger is reworked (see item 5). |
| A5 | Queen first split | Recorded, no change | Agree | — |
| A6 | REACH_UNIFY | **Partly done** (`REACH_EST`: leaders use the estimated length) | Viable, modified: **do not** give leaders the queen's paid reach | The queen's paid reach is up to 24 cells (free + one cell per segment paid). As a penalty zone for every leader and giant it would freeze our long dragons near any long enemy, and Q3 / BAND_COMPARE already showed how over-caution loses Portals. Keep one canonical *function* with a mode argument (free-only for leaders, free + paid for the queen) so the semantics are explicit and consistent. That is the safe version of "one definition". |
| A7 | Regroup protection | **Done** (portal / push / defend keep I_REGROUP) | Viable | — |
| A8 | CONTACT_SEARCH vs sprint | Open | Viable, required | Simplest fix: skip the free-step sprint extension on turns where the search changed the queen's move. About 10 lines. |
| D1 | `intent_locked()`: "every later stage starts with `if (intent_locked(why)) return`" | Not done | Viable, **modified** | A blanket early-return changes more than the known overwrites. Some current overrides are *intended*: `tg_rescue` overriding I_GUARD (a guard strikes the stalker next to our queen); `tg_hunt_queen` / cork over pearls; `tg_pack` over I_HUNT. Build it as a table: for each locked intent, the steps still allowed to override it. Then the trace diff should show only the known overwrite cases (your own stage A/B plan, which is right). |
| D2 | Rename `tn.hunter` | **Done** (`tn.opportunist`) | — | — |
| D3 | One reach definition | = A6 | see A6 | — |

## Part III — Config liveness
**Done**: comments rewritten, TIE_MARGIN removed, ADAPT_SPLIT annotated, `tools/testing/live_config.py` added.
Still open: the "live configuration" section in HANDOFF.md, and a rule to rerun `live_config.py` after config edits.
Note: `live_config.py` only finds dead code the compiler can prove; runtime-dead settings like ADAPT_SPLIT need the
short manual list kept in `Battlecode_contradictions_final.md`.

---

## Part V — New strategic features

| # | Feature | Verdict | Why | CPU | Risk | Expected value |
|---|---|---|---|---|---|---|
| 1 | Congestion-aware Manhattan matching (`cost = manhattan + λ·assigned near target`) | **Viable** | Keeps the Manhattan cost that measured best and adds the spread that the evidence says matters. Implementation: in the greedy pass, after each assignment raise the cost of other pearls within r of it, then re-pick (incremental greedy, ≤ 60 pearls × 24 agents). Every dragon computes it the same way from what it knows (the same consistency as today). | ~0.1M | Low-medium: may send dragons past a near pearl to a far one (we lost races that way before) | **Medium**, the best of the new strategy items. Test λ at two values. |
| 2 | Regret-ordered matching | **Doubtful** | Regret ordering makes the assignment closer to optimal, the same direction as MATCH_SWAP (closer to optimal, more crowding). Its benefit also assumes every dragon sees the same pearls and teammates, which they don't. | ~0 | Medium | Low |
| 3 | Region-level allocation | **Mostly exists** | ZONES (`choose_zone`: richness ÷ (teammates working it + 1) − walk), OUTPOSTS and REGION_VALUE already do soft-capacity region allocation. What's new is only the bid terms (portal value, enemy pressure). That is a re-tune of `choose_zone`, not a new layer. | ~0 | Medium (zone weights were sensitive: RV_PEARL_W hurt Portals) | Low-medium |
| 4 | Enemy reachable-set / belief map (every cell an enemy could be in) | **Viable, modified** | A full reachable set grows by the enemy's free steps every round. It is the same ballooning circle that failed as Q3 (5 Portals losses) when the queen read it. The only pruning available is **our own 7×7 view**: sonar carries sightings, never "I looked here and nothing was there", and symmetry or death evidence rarely constrains a live enemy. Viable form: (a) a reachable set **only for hunters** ("where could their queen be"), an upper bound that never feeds the queen's caution; (b) a few *likely* positions (moved by simple rules, discarded by our view) for the queen's danger map, i.e. R2 from the research audit. | sets: ~2–5M per round; guesses: ~0.05M | (a) low; (b) medium (Portals) | (a) low-medium; (b) medium-high |
| 5 | Escape-set minimisation / herding (blocker, blocker, attacker) | **Viable, modified, ladder-only** | Needs: their queen in view (true 13,986 dragon-turns locally) plus coordinated roles. Coordination: hunters seeing the same queen can split roles **deterministically by id among the hunters each one sees**, with no messages, but two hunters may see different sets and pick clashing roles. Simplified viable form: each hunter picks the move that minimises **her** escape cells within 2 turns, counting the other hunters in view as blockers. It is the contact search engine with roles swapped (R7). It also fixes A4's real problem by replacing the cork trigger. | ~5–10M when it triggers | Medium; can't be validated locally (hunting their queen is rare in the result table) | Medium (ladder) |
| 6 | Selective local minimax (CONTACT_SEARCH) | **Exists (OFF)**; viable with A8 | Your candidate counts match what's built. Two triggers from your list are already covered by exact rules: "kill available" (strike reflexes) and "interception possible" (`tg_guard` / intercept). Remaining work: A8 fix, then the benchmark (it was interrupted twice by the iCloud problem). | ≤ 1M | Medium | Medium-high |
| 7 | Deterministic role auction from "shared reconstructable state" | **Not viable fleet-wide** | The premise fails: the state isn't shared. Each dragon's teammate table differs (statuses of ordinary dragons aren't relayed; rays stop at the first dragon or kelp). Different inputs to an auction give different winners: the ROLE_CLAIMS / GIANT_GOSSIP failure again. The id-hash slots work *because* their only input (the id) is shared. Viable only **locally**: among dragons that see each other, e.g. which of two adjacent hunters takes the blocker role (item 5). | — | High | Low |
| 8 | Role hysteresis | **Doubtful until measured** | Hunter, guard and defender slots come from an id hash and don't oscillate. Giant / leader flips are the known churn, but the problem there is too many, not flipping. Goal stickiness (6c) lost Portals games 3 times. Measure role flips per dragon first (a QDIAG counter, 5 minutes) before building. | ~0 | Medium | Low |
| 9 | Short reservation table (t+0..t+3) | **Viable only locally** | Fleet-wide reservations need messages we can't afford (4 rays per turn, lossy). Local form: predict nearby teammates' next cell from their claims (or straight on), and use the turn order (lower id has already moved) for conflicts in corridors and portal mouths. That is R5 + R4 from the research audit, with portals already covered by PORTAL_ROW. | ~0.05M | Low | Low-medium (maze maps) |
| 10 | Separate tactical race field and strategic ownership field | **Doubtful** | Pearl contest already uses the cell-based Voronoi (`conf ×0.35` when they're nearer). Adding a turn-based "race probability" on top re-introduces the VOR_TURNS effect (fast long enemies → our dragons give way) unless the weight is small, and a small weight changes little. | ~0.5M | Medium | Low |
| 11 | Articulation / choke valuation | **Partly exists** | Topology has bridges, 2-edge-connected regions and branches. 7j chokepoint guards and 7g cork use them. Vertex articulation points add little on a grid (1-wide corridors are already bridges). A choke *value* to rank posts is a reasonable small extension of 7j. | ~0.2M per rebuild | Low | Low-medium |
| 12 | Split-value search (child length 2..L−2) | **Doubtful** | Splits today use fixed rules (half, keep 2, keep 3, runner keeps 5). The ladder leader (Vibing++) uses the same "split 2 off at 4". The evaluation terms (attack potential, choke holding, future value) have no reliable estimate, so the search would rank on guesses. | small | Medium | Low |
| 13 | Economic sacrifice / recycle score | **Doubtful** | RECYCLE, FEED_DELIVERY (7e) and dives already decide this. 7e (delivery rule) was the measured win; more terms means more tuning. | ~0 | Medium | Low |
| 14 | Information-value sonar | **Mostly exists** | Facts already carry priorities (queen-dead 95–99, enemy near queen 85, danger 70–75, …) and relays are prioritised. The real limit is 4 rays per turn and rays stopping at the first dragon, not the ordering. | ~0 | Low | Low |
| 15 | Zobrist + transposition tables | **Not viable / not needed** | CPU isn't the limit (44.6M of 100M; the search costs ≤ 1M). Turning saved CPU into Elo needs a search that is deeper *and* better; at depth 2 the transpositions are few. Rejected by analysis before. | — | — | ~0 |
| 16 | Local 7×7 bitboards | **Not needed** | Same reason. Timed bodies don't fit a bitmask. Rejected before. | — | — | ~0 |
| 17 | Incremental topology | **Not needed** | A topology rebuild costs ~2.7M per turn; throttling it (TOPO_EVERY 4) saved CPU but wasn't worth a behaviour change. | — | — | ~0 |
| 18 | Selective A* / 0-1 BFS / buckets | **Not needed** | A full BFS ≈ 0.3M; a Dial bucket planner already exists (UNKNOWN_EDGE_COST). | — | — | ~0 |

## Parts VI–IX — Architecture, staging, benchmarking
- **Architecture (Part VI):** the layering is sound. Two parts can't be built as drawn under our sonar: the **role
  auction** and **fleet-wide team coordination**. They work only as local versions (dragons in mutual view, or
  priority by id).
- **Stage 0 metrics:** sensible. Several exist already: `opening_econ.py` (pearls and dragons at r30–r150), `econ.py`
  (lost races), `contact.py`, the suite totals. Missing: duplicate-target rate and route overlap. These are cheap to
  add from QDIAG logs (claims vs targets).
- **Stage 2 ordering vs v3.3.0:** v3.3.0 already contains Stage 1 and most of Stage 2, shipped with one suite instead
  of separate switch-by-switch batches, at your request. Each fix has its own switch, so any one can be switched off
  if the ladder disagrees.
- **"Do not fix contradictions before measuring whether they matter" (Part IX #11):** v3.3.0 did this for speed. If
  you want to restore that discipline: set the three switches to 0 one at a time, two seed sets each (~20 minutes per
  switch).

---

## Recommended short list (in order)

| Order | Item | Form | Why first |
|---|---|---|---|
| 1 | **A8 + CONTACT_SEARCH benchmark** | skip the sprint extension when the search changed her move; then two seed sets + the queen-death re-runs | Already built; directly targets queen deaths (the main loss cause) |
| 2 | **#1 congestion-aware Manhattan matching** | λ × (pearls already assigned within r), incremental greedy; λ ∈ {2, 4} | The only new strategy item consistent with all the measured assignment results |
| 3 | **#4(b) enemy position guesses for the queen** (+ #4(a) reachable set for hunters only) | per-enemy guesses, discarded by our view; the queen's danger map reads them | Biggest diagnosed class of queen deaths (strikers from out of view), without Q3's balloon |
| 4 | **D1 intent lock as an override table** | the locked intents + the steps allowed to override each | Removes the class of bug, not just the instances |
| 5 | **#5 escape-set herding (= cork rework)** | the contact search engine with roles swapped; replaces the cork trigger that never fires | Ladder value; can't be shown locally |
| later | #9 local reservations (R4 / R5), #11 choke values, #3 zone bid terms | — | Small gains |
| drop | #2, #7 (fleet-wide), #8 (unless flips are measured), #10, #12, #13, #15–#18 | — | Evidence or game constraints say no |

Estimated effort for the short list: about 1 day of implementation; testing ~40 minutes per item at two seed sets with
the 6-game cap.
