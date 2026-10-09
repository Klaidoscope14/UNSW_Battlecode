# What v5.1.33 did, and how to extend it

## v5.1.32 -> v5.1.33
1. **SAFE_FIELD on** (the main change). Multi-source BFS "time-to-arrive" field: every enemy head seen, or reported in
   the last 3 rounds, within 20 cells gets a BFS over the known map (unexplored = open). Turns to reach a cell =
   path distance / its free steps per turn ((len+3)/4). Ours likewise. When an enemy could be on the queen's cell
   within SFD_T = 4 turns, she walks to the cell within QF_RANGE = 8 that maximises 10*eTurns + 2*open sides
   - 0.5*distance + 3*(teammates within 3, max 2), only among cells she reaches at least 1 turn before any enemy, not
   in a pocket, not in the old threat map. Her rest spot moves there too, so 'rest' no longer walks her back.
2. **EE_MIN_UNITS 5 -> 8**: escort only with 8+ dragons (5 cost economy on the ladder).
3. **HUDDLE** coded, off.

In Tron/Battlesnake terms this is a Voronoi (who-reaches-first) field used for evasion: the queen stays inside the
region she controls.

## Weak points found in the code
- **Dead zone (bug).** When the field triggers it sets fieldDone even if it finds no cell 2+ turns safer; that also
  switches off the older QUEEN_FLEE. Then she keeps resting / pearling while an enemy closes in. Ladder v5.1.38
  Islands r199: an L2 came 11 -> 9 -> 7 -> 5 -> 3 -> 1, her jobs 'rest', 'pearl', 'rest', 'rest'. Fixed by SFD_FIX
  (v5.1.41, switch).
- **Destination only.** It scores where she goes, not the cells on the way; the path can cross enemy-first ground.
- **Point score, not space.** A cell 'far from enemies' can be a small safe island; Tron bots score the *size* of the
  region you control, and avoid articulation points.
- **Enemy speed = free steps only.** Paid steps (sprints) let long enemies reach further this turn.
- **Teammates are not obstacles in her space.** Ladder v5.1.38 Around UNSW r393: no enemy within 8, 4 of ours within 3,
  no legal move -> 'doomed' self-kill after their queen was already dead (a won game lost).
- **Only enemies seen within 3 rounds** (LH_TRACK extends this, off).

## Research in the same direction
| Source | Idea | Extension for us |
|---|---|---|
| Phillips & Likhachev, SIPP: Safe Interval Path Planning (ICRA 2011) | per cell, the time intervals it is safe; A* over (cell, interval) | check the whole flee path: each step's arrival turn must be < enemy eTurns there (path margin, not destination margin) |
| a1k0n, Google AI Challenge 2010 (Tron) post-mortem; Janzert's Tron bots | Voronoi territory: count the squares you reach first; articulation points / chambers | score a flee cell by the *size* of her reach-first region around it (flood fill within margin >= 1), penalise cut vertices |
| Schier & Wüstenbecker, Adversarial N-player Search using Locality for Battlesnake (2019); arXiv 2402.03136 | diamond flood fill for board control, ties broken by length; tails freed as snakes move | same field with bodies as timed obstacles (tails free over time) and head-on ties by length - an L2 enemy does not threaten a longer queen the same way |
| Battlesnake docs, useful algorithms | flood fill per candidate move; avoid squares within 2 of a bigger head; last resort largest component | as a last resort move into the largest reachable component - would have avoided the 'doomed' box-in |
| Pursuit-evasion with Voronoi / Apollonius safe-reachable sets (arXiv 2511.15036 and related) | evader maximises the area of its safe-reachable set; moves toward its centroid | flee target = centroid of her safe region instead of the single farthest cell |
| Fletcher et al., A Visibility-Based Escort Problem; Bhatia et al. robotic bodyguards (2015); safe herding (arXiv 2509.08460) | escorts block the threat vector / intercept between attacker and VIP | escorts stand on cells where the enemy's eTurns to her is smallest (her 'front'), not a fixed ring of 3 |

## Ranked plan
1. SFD_FIX - dead-zone fix (done as a switch, testing).
2. Space score: flood-fill size of her reach-first region (Voronoi territory) in the flee/rest choice; last resort
   largest component (fixes the box-in).
3. Path margin (SIPP-lite): the first step of the flee path must keep margin >= 1 on every cell.
4. Escorts on the threat front (bodyguard / threat vector).
