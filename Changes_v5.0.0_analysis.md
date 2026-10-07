# Analysis of Changes_v5.0.0.txt (against v5.0.1, `fleet214/main.cpp`)

Date: 2026-10-07. No code changed. Every claim below was checked against the current source or our earlier test
results (`Changes_Tracker.md`, this week's 64-game runs).

## Facts that decide most of these
- **Each dragon is a separate process.** There is no shared memory. The only link is sonar: 4 messages of 64 bits
  per turn, along rows and columns, stopped by the first dragon or kelp. A dragon that dies in its move sends nothing.
- **The map is a fixed file.** Opponents cannot change it, and at round 0 we know almost none of it (7×7 view).
- **What has moved our rating:** v2.14.10 (1723) and v2.14.11 (1770) both cut **queen deaths**. Changes that sent
  dragons away from the queen or made us more aggressive cost queen deaths every time we measured them (chokepoint
  guards as in v3: 15 → 20; ladder v2.11: snipes harmful).
- **Where queen deaths come from now (v5.0.1):** 12 in 64 sparring games, 5 of them head-on hits. Against our own
  bot (strong hunters), 14 of 15 were head-on hits.

## Verdicts

| # | Proposal | Verdict | Why (short) |
|---|---|---|---|
| 1 | Space-time reservations (MAPF) | **Reject** | No shared table is possible (separate processes); "better coordination" lost twice before |
| 2 | Feeders die at her tail | **Concern partly valid, fix wrong** | She eats with her head; drops behind her tail are mostly not eaten |
| 3 | Symmetry anomaly detection | **Not needed** | Maps can't be altered; mismatches already drop a symmetry, and direct sightings already overwrite mirrored ones |
| 4 | Threat-driven role shares | **Mostly exists; the rest is risky** | HUNT_GATE and MAJ_HUNT already do the safe half; dropping guards contradicts our data |
| 5 | Deeper queen lookahead | **Best candidate** | Targets head-on deaths, now our main queen killer; needs a bounded design |
| 6 | Pay 2–3 segments to kill / starve | **Mostly exists; the rest contradicts data** | Sprints, sprint kills and TRADE_UP exist; more aggression lost on the ladder |
| 7 | Static chokepoint grid | **Reject** | Measured this week: guards away from the queen = more queen deaths; no static map at round 0 |
| 8 | Portal scout chains | **Reject (not workable)** | The follower can't see the far end; a dead scout sends nothing |

## Details

### 1. Space-time tile reservations: reject
- **No shared table.** `reservationMap` as a global can't exist: each dragon is its own process. Sharing paths
  would have to go by sonar. That's 4 × 64 bits per turn, and only to dragons on the same row or column with nothing
  in between, so the table would be mostly empty and out of date.
- **We tried the idea twice in v3, and both lost:**
  - MATCH_PATHS (path distances in pearl matching): opening pearls by round 150 fell from 555 to 426 on Around UNSW.
  - MATCH_SWAP (pearl swaps between nearby teammates): 189-15 vs 198-6.
  - In both cases, "better" assignments crowded dragons together.
- **Stalls are already handled:** `STALL_ROUNDS` / `skipPearl` give up a pearl a dragon makes no progress on.
- **Harvest crowding is brief:** a dying feeder's body disappears the same turn, leaving only pearls.

### 2. Tail-first feeding: the concern is partly valid, the fix is not
- **The concern:** feeders crowding the queen's head. It's real, but limited in the data:
  - In the 64 sparring games with v5.0.1, queen deaths were unchanged at 12.
  - Against v5.0.0 directly, one queen died during the harvest (round 476).
  - The harvest only runs when no enemy head is within 6 of her.
  - Feeders still keep off her last free exits (`QUEEN_EXIT_GUARD`).
- **Why the fix doesn't work:** she eats with her head and moves forward. Pearls dropped behind her tail are behind
  her, so she has to turn around to reach them. In the last 40 rounds she won't chase pearls more than ~3–5 steps
  away (LATE_SAFE / LEAD_SAFE), so many tail drops would never be eaten.
- **A smaller version that keeps the benefit:**
  - Feeders die beside her, not on the 3 cells in front of her head.
  - The rule ignoring the queen-space penalty during the harvest stays only for the last 2 steps.
  - Worth trying **only if** ladder losses show queens dying during the harvest.

### 3. Symmetry anomaly detection: not needed
- **No threat.** Opponents can't change the map; it's a fixed file. Symmetry formulas were checked against every
  bundled map.
- **Validation already exists:**
  - Each candidate symmetry is dropped as soon as two directly observed edges disagree (`check_edge_symmetry`).
  - `symFailed` stops all predictions once no symmetry fits.
  - A direct sighting always overwrites a mirrored guess (`learn_edge`).
- **One small real gap:** once a symmetry is confirmed it is never revoked, even if a later sighting disagrees.
  This only matters on a map that is not exactly symmetric, and none of the maps we have is like that. Low
  priority; a 10-line fix if one ever appears.

### 4. Threat-driven role shares: mostly exists, the risky part contradicts data
- **Already there:**
  - `HUNT_GATE`: no hunting while outnumbered or in survival mode.
  - `MAJ_HUNT`: on smaller maps at ≥ 2× their numbers, everyone except guards hunts.
  - Guards only from `GUARD_MIN_UNITS`.
- **"Domination → drop all guards":** this is the opposite of what moved the rating. A queen with no guard dies to
  the one hunter that gets through, and that is the tiebreak we win on.
- **"Surrounded → all hunters become a screen":** this half is reasonable. Today, gated hunters become collectors,
  not screens. It's a possible small test.
- **Why roles come from a hash of the id:** so every dragon can work out everyone's role with no messages. Shares
  based on each dragon's own estimate of the enemy's numbers would differ between dragons, so roles would flicker.
  Any version must use a fleet-wide signal that changes slowly.

### 5. Deeper queen lookahead: the best candidate
- **Why it fits:** head-on hits are now our main queen killer, and that's exactly what a lookahead against the
  nearest enemy heads prevents.
- **Full MCTS or a 5–6-move minimax is too big and partly blind:**
  - With ~3 moves per side per turn, 6 turns is about 3^12 ≈ 530k end positions, each needing an evaluation.
  - Enemies beyond our 7×7 view aren't simulated at all.
  - Strong bots also move several dragons at once.
- **What we have:** v3.3's contact search (`fleet/contact.hpp`, R1) already does a max-min over her moves × the
  nearest enemy heads' replies × her second move. It is switched off: its evaluation saturated (all values near
  1.0), and the fix was unfinished.
- **Proposal:**
  - Port it to the single file and fix the evaluation (count her safe exits and the loop she can reach, not raw
    area).
  - Run it only when an enemy head is within 6, at 2–3 turns deep against the 1–2 nearest heads.
  - Test in self-play against v5.0.1, where head-on hits are 14 of 15 queen deaths.
- **Budget:** fine. Our peak is 40M of 100M points per turn, and the search only runs in contact.
- **Cost:** the largest piece of work here. Fair warning: the v3 version didn't pay off as it was.

### 6. Pay 2–3 segments: mostly exists, the rest contradicts data
- **Already there:**
  - Long dragons already sprint several steps a turn, and a sprint plan that ends in a kill is taken outright
    (`sp.kill`).
  - `PAID_PEARL_RACE` pays one segment for a pearl race.
  - `TRADE_UP` gates strikes on worth-it targets.
- **Spending more segments on kills is more aggression.** v2.11's ladder review found snipes harmful (3W 7L), and
  survival beat attacks.
- **The only harmless piece:** allowing 2 paid steps in a pearl race. It's worth about one pearl a race, too small
  to measure.

### 7. Static chokepoint grid: reject
- **Measured this week:** guards holding a crossing up to 10 steps from the queen raised queen deaths from 15 to 20
  in 64 games. An "iron curtain" farther away is worse.
- **There's no static map at round 0:** the map is learned as we go. The topology is rebuilt whenever an edge is
  learned, and that is the only way to have one.
- **Rare in practice:** crossings that cut a map in two are rare, and portals go around them.
- **What we kept:** the safe version (v2.14.12, part of v5.0.x). One guard holds a crossing within 5 steps only
  while another guard stays by the queen.

### 8. Portal scout chains: reject (not workable)
- **The follower can't see the far end.** It's on the other side of the portal, outside the follower's 7×7 view, so
  it learns nothing from the scout's death except that it happened.
- **The dead scout sends no last report.**
- **Little to save:** scouts are already short (`scoutMaxLen`), so a lost scout costs little. A second unit
  trailing it doubles the cost.

## The questions at the end of the file
- **Which one to build:** #5 (queen lookahead), as above.
- **Topology CPU:** no problem.
  - The judge's sandbox on the largest map (big_empty) peaks at 40.0M of 100M points per turn, median 24.3M.
  - The topology rebuild is ~2.7M when edges change (measured in v3).
  - No timeouts in any test.

## Recommendation
1. **Build #5** as v5.0.2: contact search ported and fixed, enemy within 6 only, 2–3 turns deep. Test against v5.0.1
   in self-play (head-ons) and on the 64 sparring games (queen deaths must not rise).
2. **Hold #2 (light version) and #4 (hunters → screens when outnumbered)** until ladder losses show the need.
3. **Drop #1, #3, #6, #7, #8.**

---

# Follow-up proposals (lookahead fixes, one-sided tactics, small adjustments)

Checked against `fleet214/main.cpp` (v5.0.1) and the v3.3 contact search (`fleet/contact.hpp`). No code changed.

| # | Proposal | Verdict |
|---|---|---|
| 1a | Exit decay (funnel detection) in the lookahead | **Adopt as the lookahead's evaluation**, in contact only |
| 1b | Paid-reach compression | **Adopt: fixes a real gap in the v3 search.** Replies used free steps only; our threat map already has paid reach |
| 1c | Time discount | **Adopt for tie-breaks only.** A death at turn 3 must stay fatal |
| 2a | Feeders keep off the cell in front of her head | **Adopt (small).** Walking or standing there only; dying there is fine |
| 2b | Gated hunters become sensory screens | **Plausible, ladder-only test.** Local games are almost never outnumbered |
| 2c | Guards plug the corridor mouth | **Hold / reject.** Our own guard is a wall for her too |
| 3a | Survival mode collapses her leash | **Adopt (tiny)**, not in the opening |
| 3b | Queen's status sonar aimed along open lines | **Adopt (tiny), generalised** to every turn, not just corridors |

## 1a. Exit decay: adopt, inside the lookahead only
- **This is the evaluation the lookahead needs.** The v3 search scored territory and saturated near 1.0. Counting her
  real exits at each step, and how fast they shrink (3 → 2 → 1), separates "open water" from "a funnel" before the
  area runs out.
- **Partly covered already, one turn at a time:**
  - `QUEEN_LOOK2`: two-turn safe cells.
  - `PURSUIT_SAFE`: cells with one way on.
  - `pocket_ahead` / `TOPO_TRAP`: dead ends.
  - `loop_reachable`: free loops.
  The lookahead extends these over 3–4 turns.
- **Only with an enemy within 6.** Penalising every corridor while no one is near would stop her eating in narrow
  maps. Safe-ground rules (QUEEN_HOME, TOPO_TRAP) already cover the quiet case.

## 1b. Paid-reach compression: adopt
- **The gap:** the v3 contact search (`cs_enemy_bfs(e.cell, e.freeR, …)`) only let enemies move their free steps.
  But a dragon can keep moving past its free steps by paying a segment per step, down to length 2. A long hunter can
  therefore reach much farther in one turn than the search assumed.
- **We already compute paid reach:** `compute_queen_threat()` builds it for the queen every turn (`qThreat` = fewest
  paid steps an enemy needs to reach a cell; `qMargin`).
- **So:** enemy replies in the lookahead use that reach. "Near-fatal" = a cell where her distance buffer falls to 0
  within the search depth. A cheap, important fix.

## 1c. Time discount: adopt, with one rule
- **Rule: deaths are compared first, at every depth.** A line where she dies at turn 3 must lose to any line where
  she lives, whatever the discount. Otherwise the "bait" this proposal warns about (a trap that only closes at turn
  3) is exactly what gets discounted away.
- **Among the surviving lines:** turn 1–2 terms get ~70% of the weight, and turns 3–4 break ties.

## 2a. Clearance vector for feeders: adopt, small
- **We know her facing:** in view, her neck shows it. Without view, the fleet report's last two positions show it.
- **Rule:** a feeder doesn't walk or stand on the cell in front of her head.
- **Dying there is fine, even good.** A dead feeder's body disappears the same turn, so the cell becomes a pearl
  right in front of her: food, not a wall. The block only exists while the feeder is alive.
- **What exists already:** `QUEEN_EXIT_GUARD` only stops a feeder taking one of her last 2 exits. This rule is
  stricter about the front cell.
- **Expected effect: small.** In 32 games against v5.0.0, one queen died during the harvest.

## 2b. Gated hunters become sensory screens: plausible, but only the ladder can test it
- **The idea is sound.** Screens already stand 6–9 cells out on her row or column (`SCREENS`), so their sonar
  reaches her. Gated hunters would add more of them.
- **The claim that farming hunters crowd the queen is not borne out:** collectors go to pearls anywhere, not to her.
- **Cost:** fewer collectors when we are already behind on numbers.
- **Not testable locally:** neither the sparring bots nor self-play leave us outnumbered often enough. It would be
  a ladder experiment.

## 2c. Corridor plugging: hold / reject
- **Our own guard's body is a wall to the queen too.** A guard at one mouth of her corridor plus an enemy at the
  other seals her in.
- **She is rarely in corridors:** QUEEN_HOME, TOPO_TRAP and PURSUIT_SAFE keep her out.
- **The existing rule is the safer one:** guards never walk into her corridor toward her (`qCorr`).
- **Same family as the chokepoint guards:** that idea only worked once we kept a guard beside her.

## 3a. Collapse her leash in survival mode: adopt, tiny
- **The leash today:**
  - It resets to 7 on any threat within 8 (seen, or reported in the last 3 rounds).
  - It grows one cell per 2 quiet rounds, only before round 300, up to 24.
  - It is unlimited in the opening.
- **The gap:** survival mode (tiny fleet, or more than 3× their dragons) doesn't reset it if no enemy happens to be
  near.
- **Fix:** `survivalMode` → leash back to 7. One line.
- **Not in the opening:** there `units ≤ 3` counts as survival, and she has to roam to eat.
- **Expected effect: small and rare,** but it can't hurt.

## 3b. Sonar aimed along open lines: adopt, tiny, generalised
- **The gap:** the queen sends her status twice a turn (`QUEEN_POS_RELAY`), but the directions just rotate each
  turn. In a corridor, one copy is often sent straight into a wall.
- **Fix:**
  - Aim the two status copies at directions whose first cell is open water, preferring directions where a teammate
    is in line.
  - Do this every turn, not only in corridors.
- **"Drop pearl gossip" changes little:** she already sends pearl news only when there is spare room.
- **Sonar only, no movement change, so very low risk.**

## Order (one change per submission)
1. **v5.0.2: the queen lookahead (#5)** with 1a + 1b + 1c as its design. The big one; test against v5.0.1 in
   self-play and on the 64 sparring games.
2. **v5.0.3: 3a + 3b together.** Both are tiny queen-safety fixes that no local test can tell apart, so separate
   ladder runs would add nothing.
3. **v5.0.4: 2a**, the clearance rule for feeders.
4. **Later, ladder-only: 2b.** Hold 2c.
