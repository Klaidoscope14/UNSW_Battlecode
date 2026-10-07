# Contradictions and inconsistencies: final list and fix plan

Date: 2026-10-07. Code: `fleet/` (the source of `fleet_single/main.cpp`).
Inputs: your `Battlecode_contradictions_audit.txt` (findings 1–9), a manual pass over the role, intent, split, threat
and scoring code, and two mechanical checks:
- **Config liveness** (`tools/testing/live_config.py`): every `#define` in `config.hpp` (364) is recompiled with its
  value changed and the assembly compared. Result: 322 change the bot, 42 don't. The compiler can't see runtime-dead
  cases (a flag that is always false), so those were searched for separately.
- **Intent overwrite table**: every write of `tn.target` / `tn.why` in the 21 target steps, and which later step can
  replace it.

Measured numbers are marked "measured", taken from the 204-game diagnostic runs (QDIAG logs).

---

## A. Confirmed runtime contradictions (the code really does two different things)

| ID | Source | What happens | Where | Severity (my assessment) |
|---|---|---|---|---|
| A1 | yours #1 + #7 | Hunter / guard / screen / defender flags use **last turn's** `my.leader` and `amQueen`. Leader demotion, election, heir and giant then change `my.leader` **after** the flags are set. `amQueen` is also refreshed only after use (line 166). `my.wall` is computed after the election, so it is consistent. | `situation.hpp:145–166` vs `166–250` | MEDIUM: one-turn lag on turns when leadership changes; frequency below |
| A2 | yours #2 | `sit.consolidate = CONSOLIDATE && openMap && R >= CONSOL_ROUND` is computed and then **unconditionally replaced** by `sit.switched` (DYN_ALLOC = 1). The comment above still says "small or kelp-dense maps keep their numbers", which is now false: every map consolidates after the switch. `sit.openMap` itself is still used elsewhere (HALF_SPLIT, CAP_FEED, feeding). | `situation.hpp:264–281` | MEDIUM as code (misleading); the behaviour is the deliberate DYN_ALLOC design |
| A3 | yours #3 | `tg_guard` sets `target = -1, why = I_POST`; `tg_explore` doesn't exclude I_POST and gives the guard an explore goal. **Narrower than reported:** outside the post the post cell becomes the target (no overwrite); inside the post at length ≤ 3 the forced circling step (+900k) outweighs any explore goal. **Real case:** inside the post at length ≥ 4 with the trim split refused (unit cap or the child can't escape): the guard walks off toward an explore goal. | `decide.hpp:1551–1586`, `1975` | LOW-MEDIUM |
| A4 | **new** | **Cork is overwritten.** `tg_cork` (7g) sets `I_CORK`. The later `tg_pack` replaces any target that isn't a near pearl (`target < 0 \|\| why != I_PEARL \|\| ...`), so a pack hunter loses the cork the same turn and chases their queen instead. `tg_guard` and `tg_feed` can also replace it. Locally the cork never sets a target (measured: 0 cork targets in 204 games; their queen was seen in a corridor 758 times), so this only matters on the ladder. | `decide.hpp:1153–1218` vs `1359–1369`, `1448+`, `1222+` | MEDIUM (ladder) |
| A5 | **new** | **The queen's first split can't happen at the start.** `QUEEN_FIRST_SPLIT` ("with ≤ 3 dragons the queen may split at length 4") is blocked by the `crowded` check, which counts **every** head within 3, including our own starting dragons beside her. Measured: the queen sat at length 4 until round 150 on Australia. (`QUEEN_START_SPLIT` tested the fix: more queen deaths, so the blocked behaviour is the one that tested better.) | `decide.hpp:2208–2240` | MEDIUM as a contradiction; the current behaviour tested better |
| A6 | **new** | **Two different enemy-reach models.** The queen's danger map uses free + paid sprint steps and `max(len, estLen)` (bodies partly out of view). Leaders and giants (`end_threat`, the score's head term) use `enemy_reach(h)` = free steps only, from the **visible** length `h.len`. So a long leader underestimates a half-visible enemy. Non-leaders use reach 1. | `fields.hpp:152`, `tactics.hpp:131`, `decide.hpp:2518` vs `fields.hpp:206–270` | MEDIUM |
| A7 | **new** | **Regroup is not protected either.** `tg_pack` sets `target = -1, why = I_REGROUP`. Later steps that only test `target < 0` can replace it: `tg_portal` (portal-role dragons), `tg_push` (expansion hunters), `tg_defend` (endgame packs). `tg_explore` does exclude it, so protection is inconsistent across steps (your #8, made concrete). | `decide.hpp:1419–1420` vs `1822`, `1877`, `1896` | LOW |
| A8 | **new** (R1, still OFF) | When the new contact search chooses the queen's step, `finish_move` may still extend it into a free-step sprint. That sprint is judged by its own score (pearls, area, end threat), not by the search. So the two can disagree about where she ends the turn. | `decide.hpp` contact block vs `finish_move` | LOW now; to fix inside R1 before it is turned on |

## B. Settings that have no effect in the current build

### B1. Overridden by another switch, though they look active (all nonzero)

| ID | Switch | Overridden by | Effect of changing it | Source |
|---|---|---|---|---|
| B1a | `QUEEN_GROW_ROUND` 225 | DYN_ALLOC (`queenSplits = !switched \|\| ...`) | none | yours #4 |
| B1b | `EXP_END` 350 | DYN_ALLOC (`growth_open = !switched`) | none | yours #5 |
| B1c | `EXP_DENSITY` 12 | DYN_ALLOC (`expansion_target` uses DYN_DENSITY) | none | new (compile check) |
| B1d | `CONSOL_MIN_UNITS` 10 | DYN_ALLOC (`cons_floor`) | none | new (compile check) |
| B1e | `CONSOLIDATE` 1, `CONSOL_ROUND` 350 | DYN_ALLOC overwrite (A2) | none (runtime-dead; the compiler keeps the dead store) | new (by reading) |
| B1f | `QUEEN_SEED_UNITS` 3 | EXPANSION = 1 (the `else` branch of the queen split is never taken) | none | new (compile check) |
| B1g | `ADAPT_SPLIT` 1 | QUEEN_ADAPT = 0 (`sit.queenOpening` is always false) | none (runtime-dead; the compiler can't see it) | yours #6 |
| B1h | `EARLY_SPLIT_ROUNDS` 150 | EXPANSION = 1 for the main growth-split rule; **still live** in the SPLIT_MORE hunter clause | partial | new |
| B1i | `UNKNOWN_EDGE_COST` 1 | itself: the Dial planner runs only when > 1 | off at 1 (fine, but the comment doesn't say so) | new |

### B2. Orphan
| ID | Switch | Finding |
|---|---|---|
| B2a | `TIE_MARGIN` 5 | defined, **used nowhere** (the comment describes a rule that no longer exists) |

### B3. Parameters of features that are switched OFF (expected; listed so nobody tunes them by mistake)
- Contact search: `CS_*` (12).
- `GOSSIP_MAX_AGE`, `GOSSIP_AGE_UNIT`.
- `STRIKE_CONFIRM`.
- Opening tests: `HOLD_*`, `OQS_END`, `QSS_END`, `OPEN_PUSH_END`, `OPEN_HOLD_END`.
- Queen adapt: `QUEEN_ISO_RAD`, `THREAT_RADIUS`, `QUIET_ROUNDS`, `LEASH_MAX`, `ADAPT_END_ROUND`, `ADAPT_OPEN_END`.
- `PORTAL_RUSH_DIST`, `PORTAL_RUSH_PEARL`.
- `MATCH_DEPTH`, `MATCH_SELF_SAME`, `MATCH_SWAP_RANGE`.
- `PAID_THREAT_PEN` (QUEEN_HARD_THREAT on).
- `QUEEN_CLEAR_PEN`, `QUEEN_ROOM_PEN` (their features are off).

## C. Misleading comments (code and comment disagree)

| ID | Where | Says | Actually |
|---|---|---|---|
| C1 | `situation.hpp:264–266` | "small or kelp-dense maps keep their numbers" | every map consolidates after the switch (A2) |
| C2 | `config.hpp` OPEN_PLAN | "the queen splits down to 2 and explores when no teammate is near her" | that part is OPEN_QUEEN, which is OFF |
| C3 | `config.hpp` ADAPT_SPLIT | "in the opening the queen splits at length 4" | dead (B1g) |
| C4 | `config.hpp` QUEEN_GROW_ROUND, EXP_END, EXP_DENSITY, CONSOLIDATE / CONSOL_ROUND / CONSOL_MIN_UNITS, QUEEN_SEED_UNITS | describe active rules | only active with DYN_ALLOC 0 / EXPANSION 0 (B1) |
| C5 | `config.hpp` QUEEN_FIRST_SPLIT | "with ≤ 3 dragons the queen may split at length 4" | blocked at the start by her own crowd (A5) |
| C6 | `config.hpp` TIE_MARGIN | describes a safe-lead rule | unused (B2a) |

## D. Structural risks (your #8 and #9, agreed)

| ID | Finding | Concrete instances found |
|---|---|---|
| D1 | Intent is one shared, mutable `target` / `why` written by 21 target steps in order. Each step protects only the intents it remembers to check. | A3 (I_POST), A4 (I_CORK), A7 (I_REGROUP). Protected inconsistently: `tg_explore` checks GUARD / REGROUP / EVADE; `tg_rescue` checks INTERCEPT / POST; `tg_defend` checks INTERCEPT only; `tg_pack` / `tg_wall` / `tg_defend` protect only near pearls. |
| D2 | Roles are not derived in one final pass from the final leader state. | A1 |
| D3 | Many switches are overridden by `DYN_ALLOC` / `EXPANSION` (whole older systems kept alive for comparison). | B1 |
| D4 | Two "hunter" concepts with one name: `my.hunter` (slot-based pack hunter, situation.hpp) and `tn.hunter` (opportunistic, `tg_hunt`: combat-role or outnumbering short dragons). | naming only, but easy to misread |
| D5 | No single definition of "enemy strike reach". | A6 |

## E. Checked and NOT a contradiction (so nobody re-checks them)
- `my.wall` is computed after the election: consistent.
- `ignoreContested`: every set is paired with a reset.
- I_LATE_SAFE: `tg_explore` can't overwrite it (`queenStarving` excludes the late rounds).
- I_BUNKER: the forced loop (+900k) dominates any explore target.
- `cost.set()` for unknown portal exits replaces earlier terms on purpose (none apply to unknown exits).
- Feeding start under DYN_ALLOC: consistent (`feedStart = switchRound` once switched).
- `amQueen` before `compute_fleet_state` (perception): `queenId` only changes in the first rounds; no effect after.

## Status in v3.3.0 (2026-10-07)
| Item | Done | How | Switch (v3.3.0 value) |
|---|---|---|---|
| C1-C6, B2a, A2 code, D4 | yes | stage 1 clean-up; trace IDENTICAL to v3.2.0 (24 games) | - |
| A1 role order | yes | duties derived after the election, `amQueen` first | `ROLE_ORDER_FIX 1` |
| A3 guard post | yes | `tg_explore` keeps I_POST | `INTENT_GUARDS 1` |
| A4 cork | yes | `tg_pack`, `tg_feed` keep I_CORK (guards still take priority) | `INTENT_GUARDS 1` |
| A7 regroup | yes | `tg_portal`, `tg_push`, `tg_defend` keep I_REGROUP | `INTENT_GUARDS 1` |
| A6 enemy reach | yes (partly) | leaders use the estimated length; still free steps only (paid reach of 20+ cells would freeze long dragons) | `REACH_EST 1` |
| A5 queen first split | no change | recorded; unblocking it tested worse | - |
| A8 contact search + sprint | open | R1 is OFF in v3.3.0 | - |
| D1 general intent lock | partly | the three protections above; no general helper yet | - |

---

## Fix plan

Two kinds of fix, kept apart as you asked: the current tuning may depend on today's behaviour, so behaviour changes
are tested separately from clean-ups.

### Stage 1: no behaviour change (verified by the decision trace: must stay IDENTICAL)
1. **C1–C6 comments**: rewrite them to say what the code does now, including "only with DYN_ALLOC 0" on every B1
   switch.
2. **B2a**: delete `TIE_MARGIN`.
3. **A2 / B1e code**: wrap the open-map consolidate formula in `if (!DYN_ALLOC)` so it is visibly the DYN_ALLOC-off
   path. `sit.openMap` stays computed (it is used elsewhere).
4. **B1g**: annotate `ADAPT_SPLIT` "needs QUEEN_ADAPT". Optionally set it to 0 to match reality; play is identical.
5. **D4**: rename `tn.hunter` → `tn.opportunist` (decide.hpp only).
6. **Guard against recurrence**: run `tools/testing/live_config.py` after every config change. Add a short "live
   configuration" section to HANDOFF.md listing the B1 overrides. Optionally make `check_switches.py` also flag
   switches used nowhere (would have caught TIE_MARGIN).

### Stage 2: behaviour fixes, each behind its own switch (default OFF), suite on seeds 1–2 and 3–4
| Fix | Change | Switch | Expected effect | Notes |
|---|---|---|---|---|
| F-A1 | Reorder `compute_fleet_state`: `amQueen` first, then leader demotion / election / heir / giant, then derive hunter / guard / screen / defender / wall from the final values | `ROLE_ORDER_FIX` | small (only the change turns, see measurement) | the counts tell us whether it's worth testing at all |
| F-A3 | `tg_explore` (and `tg_portal` / `tg_push` / `tg_defend`) skip I_POST; when the post trim split is refused, hold the post: `postDir` = the post's loop step at any length | `POST_HOLD` | small; guards stay at the bunker | |
| F-A4 | Protect I_CORK: `tg_pack`, `tg_feed`, `tg_guard` keep a cork target (or move `tg_cork` after `tg_pack`) | `CORK_KEEP` | locally none (the cork never fires); ladder only | ship with a ladder trial |
| F-A6 | One `enemy_strike_reach(h)` (free + paid steps, `max(len, estLen)`) used by `end_threat` and the score's head term for leaders / giants; non-leaders keep reach 1 | `REACH_UNIFY` | leaders and giants become more careful near half-visible long enemies | the most likely of these to matter; watch Portals |
| F-A7 | Make I_REGROUP protected the same way as I_POST (folded into D1's helper) | via D1 | tiny | |
| F-A8 | In R1: skip the free-step sprint extension when the contact search changed the queen's move, or add sprint endpoints as search actions | part of `CONTACT_SEARCH` | needed before R1 can go on | do it in R1's next iteration |
| F-A5 | **No code change recommended.** Record that QUEEN_FIRST_SPLIT is effectively off at the start; unblocking it is what QUEEN_START_SPLIT tested (+14 queen deaths over 408 games). | — | — | your call |

### Stage 3: the structural fix for D1 (one change, after stage 2)
- Add `bool intent_locked(Intent w)` returning true for the intents that must not be replaced by later economy or
  explore steps: POST, GUARD, REGROUP, EVADE, CORK, LATE_SAFE, BUNKER, INTERCEPT, FLEE.
- Every target step after `tg_guard` checks it at its entry, instead of each step carrying its own partial list.
- Test: first run the decision trace to list which games change (they should be exactly the overwrite cases above),
  then the suite on both seed sets.

### Order and gates
1. Stage 1: the trace must be identical, then commit point.
2. Measure A1's frequency (debug counter added; results below) and decide whether F-A1 is worth a suite.
3. F-A6, F-A3, F-A4, each with suites on both seed sets (protected-map rule on each seed set).
4. Stage 3 (D1 helper).
5. F-A8 together with the next R1 iteration.

---

## Measurement for A1 (debug-only counter, 36 games)
Not measured: cut to ship v3.3.0 quickly (the user's call). A1 is fixed in v3.3.0 by `ROLE_ORDER_FIX`.
