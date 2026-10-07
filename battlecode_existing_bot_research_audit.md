# Research audit of the fleet bot (v3.2.0)

Date: 2026-10-07. Scope: the code in `fleet/` (18 files, ~10,100 lines) and the 13 papers in `Research_Papers/`.
No code was changed for this audit.

How to read the numbers:
- **Measured** = from our own test runs: the 204-game suite (`results/sV320.txt`), its second-seed run (`sV320b_s3`) and
  the 204-game diagnostic run with queen logging (`QDIAG`).
- **Estimated** = my own calculation, marked as such.
- CPU is in judge "points": 100M per dragon per turn, 1 point per basic operation, 2 per memory access, 4 per call,
  nothing carried over. Memory limit 48 MB.

---

## 1. Executive summary

**Verdict.** The bot's one-turn tactics are already close to exact:
- strike paths (BFS through visible cells),
- sprint planning (exhaustive DFS over at most 3 free steps),
- time-aware body occupancy,
- the queen's one-turn danger map, which is exact because every enemy dragon moves after our queen.

Plain MCTS would not beat these. Its weaknesses are elsewhere:

1. **The queen's choices when enemies are close** come from a fixed sum of hand-set penalties: threat, room, corners,
   two-turn exposure, crowd flight. Our own tests show this sum is fragile in both directions:
   - every change that made her more cautious made her give up room and trap herself (bands 6a, Q3, 7h);
   - every change that gave her more freedom raised queen deaths (QUEEN_ADAPT, 7d, OPEN_QUEEN, QUEEN_START_SPLIT).

   A fixed weight can't settle "room vs threat". Looking at what actually happens a few moves later can.
2. **Enemies out of view** are either forgotten after 3 rounds or modelled by a reach circle that grows each round. The
   growing circle made the queen too timid (Q3: 5 Portals losses); forgetting leaves her blind to long strikers, the
   largest class of queen deaths in the diagnosis.
3. **No opponent model.** Ladder opponents use kamikaze sprints; local sparring bots rarely attack. One fixed caution
   level can't suit both, and that is why local results and ladder results keep pulling in opposite directions.
4. **Coordination** is limited by sonar, not by algorithms: only facts that never expire spread fleet-wide (Q1 vs
   6d/6d''). Algorithms that assume a shared plan (CBS, centralised matching) can't run here.

**What the papers support.** The Tron papers are the closest relatives of this game:
- In the 2010 Tron competition, plain Monte Carlo entries ranked 25–200. The winners used minimax with a territory
  evaluation, and the best tree-search entry dropped random playouts for a heuristic.
- With short time limits, deterministic selection beats stochastic selection (Perick et al.).
- Mixing heuristic minimax values into a search works best with a weight of 0.15–0.4 (Lanctot et al.).
- POMCP's lasting idea for us is its belief update (a particle filter that discards guesses contradicted by what we
  see), not its search tree.

**Recommendation in one line.** Keep the architecture. Add:
- a **selective, deterministic, two-ply adversarial "contact search" for the queen**, with a Tron-style
  territory-and-loop evaluation, blended into her existing score (not replacing it);
- **particle-style enemy position guesses** for enemies that have left view;
- a cheap **in-game opponent aggression estimate** that picks between two caution profiles we have already tested.

MCTS proper: **only in specific situations**, and even there a deterministic shallow search is the better first
choice at our budget.

---

## 2. Existing bot architecture

One translation unit: `main.cpp` includes the layers in order. Every dragon runs its own copy, and dragons share
nothing except 4 sonar messages a turn.

```
main.cpp turn loop
  read_vision -> update_body -> read_sonar -> update_unobserved -> resolve_symmetry -> prune_knowledge
  -> detect_teammate_deaths -> record_enemies -> [strike_news_check, off] -> compute_fleet_state
  -> decide():
       prepare_turn        distances (plan_bfs), threat maps, periodic news (zones, phase, queen-dead)
       analyse_moves       4 candidates: legality, flood area, 2-step ok, survival search (queen), pocket /
                           dead-end / topology traps, dive values, loop reachability
       read_contact        adjacent heads, stalkers, which head-on trades we want
       reflex table        (first that acts ends the turn) regicide (paid/sprint), escort hit, snipe,
                           pack strike, intercept*, queen paid escape*, no-legal-move, escape sprint,
                           trap split, insurance split, recycle, attack          (* = OFF)
       target steps        bunker watch, pearl (greedy matching), queen caution, hunt, hunt queen, cork,
                           feed, pack, wall, guard, rescue, queen bunker, queen rest, portal, push, defend,
                           opening camp, zone, outpost, explore, commit
       feed_sacrifice, plan_split
       score_moves         ~60 additive terms tagged LIFE / SAFETY / GOAL / STYLE (plain sum, BAND_COMPARE 0)
       finish_move         free-step sprint extension (sprint_dfs), queen sprint survival check
  -> send_sonars (4 rays, encrypted)
```

### A. World model (`world.hpp`, `perceive.hpp`)

| Item | Implementation |
|---|---|
| Edges | `edgeType` per edge: unknown, open, kelp or portal. `cross()` treats unknown as not crossable; `step_open()` treats unknown as open; `flood()` reports a "frontier". The rules are documented as "GraphView". |
| Kelp | edge type plus a `kelpList` passed on as gossip |
| Portals | `edgePid`, `edgePartner`, `portalById`; heading is kept through a portal (checked in the engine) |
| Symmetry | `symMask` holds 3 candidate symmetries, tested on observed edges and on shared countdowns; `mirror_tile` |
| Pearls | `pearlState` (present / predicted), `obsNext` / `nextSpawn` / `nextEst`, respawn gaps `gapLo` / `gapHi`, fountains, death drops |
| Enemy observations | `heads` (this turn), `enemyReps` (reports for up to 200 enemies), `enemyLenMem` (length memory, separate rules for the queen), `estLen` for bodies seen only partly |
| Teammates | `fleet` map (id → length, cell, round, leader), from STATUS messages; queen and leader statuses are relayed |
| Persistence | Everything above lasts the whole game; facts are passed on as gossip with priorities |

### B. Perception (`perceive.hpp`)
- **7×7 vision.** `read_vision` marks visible cells and occupants. Enemy bodies are followed from the head to give
  exact or partial lengths. `otherRel` releases a body cell at the time its tail will have moved, if the whole body is
  visible; otherwise the cell stays solid.
- **"Contested" cells**: cells next to an enemy head.
- **Own body.** `update_body` follows the visible chain and replays last turn's steps for the part out of view
  (exact through portals). B1b: a visible empty cell is always legal.
- **Sonar.** `read_sonar` decrypts and checks each message, applies it, and relays new facts with priorities.
- **Death detection.**
  - Teammates: a teammate that vanished from the middle of our view.
  - Their queen: she vanished in view, or a death drop appeared on her last body. Strict rule, 0 false alarms; the Q1
    "she is dead" bit spreads on every STATUS.
- **`update_unobserved`**: tiles out of view whose countdown ran out probably have a pearl.

### C. Search and graph algorithms (`graph.hpp`, `topology.hpp`, `act.hpp`, `tactics.hpp`)

| Search | Where | What it is |
|---|---|---|
| `flood` | graph | BFS with timed bodies; area, tail reached, frontier |
| `plan_bfs` | graph | single-source distances with timed bodies (Dial variant when unknown edges cost more) |
| `survive_rec` / `queen_can_survive` | graph | DFS for a path of 2L+6 moves (at most 50) through water, the queen's own trail blocking, 20k nodes |
| `dive_rec` | graph | dead-end mining: do the pearls pay for the survival split? depth 12, 4k nodes |
| `sprint_dfs` | act | exhaustive search over at most 3 steps (free + paid), flood at the end of each sequence, kill detection |
| `regicide_path` | tactics | BFS strike path through visible cells within free + paid reach |
| `look2_safe_cells` | tactics | queen: cells she can reach next turn that no enemy reaches within 2 turns |
| `topo_build` | topology | Tarjan bridges, 2-edge-connected loop regions, bridge forest, Branch summaries; `topo_trap`, `topo_room`, `topo_loop_near` |
| pocket / `known_dead_end` / `loop_reachable` / `ring_free` / `corridor_leads_to` | topology | local pocket and loop checks |

### D. Spatial fields (`fields.hpp`)
- Region value (pearl richness), recomputed every 5 rounds.
- Voronoi `vorOurs` / `vorEnemy`: multi-source BFS from our heads and theirs (reports up to 3 rounds old).
- Queen threat map `qThreat`: fewest paid steps any enemy needs to reach a cell. Also the number of attackers per
  cell, the margin just outside each reach, and `qThreat2` (two turns). Reports aged 1–3 rounds widen the reach.
- `qDist`: distances from the queen.
- Mobility: `open_sides`, `queen_exits`, flood area per candidate.

### E. Team strategy (`situation.hpp`, `commander.hpp`, `intent.hpp`)
- **Roles by id hash**: collector, explorer, portal, combat. Slot plan: hunter, guard, screen, defender, wall.
- **Phases**: expansion, then DYN_ALLOC switch (numbers → length), transition, consolidation; OPEN_PLAN until we have
  the majority.
- **Leaders**: up to 3 elected; one giant / heir from length 5 (surplus measured: median 9 giants at once).
- **Hunters**: packs on their queen or long enemies (`assign_prey`), screens and guards, chokepoint guards (7j),
  cork (7g), wall formation.
- **Feeding**: sacrifice next to the queen (7e delivery rule).
- **Endgame**: bunker (length ≤ 3, 2×2 loop), heir, giant hunt, late-safe, survival mode.

### F. Coordination (`commander.hpp`, `comms.hpp`, `speak.hpp`)
- **Implicit, deterministic**: each dragon solves the same greedy pearl matching (Manhattan distance, claims), prey
  assignment, and id-hash slots.
- **Sonar** (16 message types, encrypted): STATUS every turn (carries the "their queen is dead" bit), pearls, portals,
  kelp, enemies, dangers, symmetry, claims, zones, phase, bunker, queen-dead; relayed by priority.
- **Ordering rules**: on portals the lower id goes first (`PORTAL_ROW_PEN`); teammates keep out of a 1-wide corridor
  the queen is in (`CORRIDOR_GUARD`).

### G. Action selection
The pipeline the request describes is the real one:

perception → fleet state → `analyse_moves` → `read_contact` → reflexes → target steps → feed / split → `score_moves`
(additive) → `finish_move` (sprint extension) → action.

**No part of it reasons about what an enemy will do next**, apart from the worst-case reach maps.

**Turn-order fact that shapes every recommendation.** Dragons act one at a time in ascending id order, and the queens
are ids 0 and 1. So between two turns of one dragon, every other dragon moves exactly once, and every enemy dragon
moves after our queen. From the queen's point of view the game alternates: her move, then a reply from each enemy.
The one-ply danger map is therefore exact; deeper plies are where a model of the enemies matters.

---

## 3. Current strengths
1. **Exact one-turn tactics**: strike paths, sprint DFS, timed bodies, exact one-ply danger (queens move first).
2. **Static map understanding**: bridges, loop regions, dead-end branches (7b), pockets, fountains, symmetry and spawn
   prediction.
3. **Robust implicit coordination**: greedy Manhattan matching. "Better" matchings (MATCH_PATHS, MATCH_SWAP) were
   measured to be worse because they crowd dragons together.
4. **Engineering**: identity checks by decision trace, a 204-game suite, every feature behind a switch, a written
   record of every tested change, and CPU headroom (max 49.7M of 100M points).
5. **Endgame rules** that win the round-500 tiebreak: bunker, heir, late-safe.

## 4. Current limitations
1. **Horizon**: non-queen moves see 2 steps plus static topology. The queen sees 2 turns as a worst-case union plus a
   single-agent survival search in which enemies do not move.
2. **Additive trade-offs in contact**: ~60 hand-set terms, one plain sum. Fragile, as shown by the test history.
3. **No enemy intent**: every enemy is assumed to go anywhere it can reach.
4. **Unseen enemies**: reports expire after 3 rounds; a growing circle balloons.
5. **No opponent adaptation within a game.**
6. **Coordination bandwidth**: 4 messages per turn, rays stop at the first dragon or kelp, and the sonar network is in
   pieces on kelp-heavy maps (measured with the 6d'' gossip). Fleet-wide agreement on anything that changes is
   unreliable.
7. **Testing**: the local suite is saturated (196–198/204, noise ±3 wins, ±5 queen deaths), and the protected-map rule
   is noisy. v3.2.0 itself loses 6 Portals games on seeds 3–4.

---

## 5. Paper-by-paper applicability

### 5.1 Kocsis & Szepesvári 2006 — "Bandit based Monte-Carlo planning" (UCT)
- **Core idea**: treat each tree node as a multi-armed bandit and pick children by UCB1; converges to the optimal
  choice given enough simulations.
- **What my bot already has**: exhaustive search where the branching is small (sprints ≤ 3 steps, strikes), and
  heuristic scoring elsewhere.
- **What this paper adds**: anytime best-first search using simulations.
- **Battlecode compatibility**: poor as given. It assumes a full simulator of everyone's moves and a known state;
  ours is partially observed with up to ~60 moving agents, and the opponent's policy is unknown.
- **CPU cost**: (estimated) one useful playout of 6–10 plies with nearby dragons and a final flood costs 30–80k
  points, so 300–1000 playouts per turn at most. The papers' gains appear at 10³–10⁴.
- **Memory cost**: small (a tree of a few thousand nodes, < 1 MB).
- **Implementation complexity**: medium (simulator plus tree).
- **Expected strategic value**: low on its own.
- **Recommended adaptation**: none directly. It is the base of the variants in section 7.
- **Exact files/modules affected**: none.
- **Priority**: do not implement.

### 5.2 Gelly & Silver 2011 — MCTS and RAVE in Go
- **Core idea**: share action values across a subtree ("all moves as first") and blend them with the Monte Carlo
  value on a schedule (β); add prior knowledge to new nodes.
- **What my bot already has**: nothing similar; no tree.
- **What this paper adds**: faster learning when a move's value is roughly the same whenever it is played.
- **Battlecode compatibility**: the core assumption fails. "Move north" has no value independent of position; a
  dragon's moves are relative directions on a changing board. Go stones are placements on fixed points, which is what
  makes RAVE work.
- **CPU cost**: as for UCT, plus the shared-value bookkeeping.
- **Memory cost**: moderate.
- **Implementation complexity**: medium-high.
- **Expected strategic value**: none.
- **Recommended adaptation**: none. The paper's results use 3,000 simulations per move.
- **Exact files/modules affected**: none.
- **Priority**: do not implement.

### 5.3 Chaslot et al. 2008 — Progressive strategies for MCTS
- **Core idea**:
  - progressive bias: add a heuristic term H/(n+1) that fades as visits grow;
  - progressive unpruning (widening): start with the top-k heuristic children and admit more as visits grow;
  - spend expensive heuristics only on nodes visited enough.
- **What my bot already has**: heuristic move ordering in places (the sprint DFS evaluates in order, strike BFS).
- **What this paper adds**: a principled way to **reduce the move space** and to **spend expensive evaluation only
  where the search goes**.
- **Battlecode compatibility**: the concepts transfer well to any selective search. Enemy reply sets are large (every
  cell within reach), and widening is the right tool for them.
- **CPU cost**: lowers cost; it is a pruning rule.
- **Memory cost**: none.
- **Implementation complexity**: low (an ordering plus a top-k cap).
- **Expected strategic value**: enabling, for the contact search (R1).
- **Recommended adaptation**: in R1, order each enemy's reply cells by how threatening they are (on the queen's next
  exits, cuts her territory, cells she must pass) and keep the top k = 4–6. Widen to 8 when the node budget allows.
- **Exact files/modules affected**: new `contact.hpp` (R1).
- **Priority**: use inside R1.

### 5.4 Winands, Björnsson & Saito 2008 — MCTS-Solver
- **Core idea**: pass proven wins and losses up the tree; a proven loss is never chosen again. "Secure child" picks
  the final move. Beats plain MCTS 65% in Lines of Action, a sudden-death game.
- **What my bot already has**: hard death detection at one ply (a move inside the free reach costs 3e6; dead ends
  cost 2.5e6).
- **What this paper adds**: proven outcomes inside a deeper search. Our game is sudden-death for the queen: one
  head-on ends it.
- **Battlecode compatibility**: high for queen safety. "Queen dead" is a terminal loss we can prove under worst-case
  enemy replies.
- **CPU cost**: none (bookkeeping).
- **Memory cost**: one byte per node.
- **Implementation complexity**: low inside R1.
- **Expected strategic value**: medium inside R1. Prunes certain-death branches early and frees budget for the real
  choices.
- **Recommended adaptation**: in R1, mark a queen move as a proven loss when any modelled enemy reply reaches her
  head. Prune it, unless every move is a proven loss, in which case fall back to the existing insurance split / stall
  reflexes.
- **Exact files/modules affected**: `contact.hpp` (R1).
- **Priority**: use inside R1.

### 5.5 Lanctot et al. 2014 — MCTS with implicit minimax backups
- **Core idea**: keep heuristic minimax values separate from playout averages and select with
  (1−α)·(playout average) + α·(minimax value); α between 0.15 and 0.4 is a safe range. In their games, traps are
  handled mostly by a playout policy that takes winning moves and avoids immediate losses (94% vs random playouts).
- **What my bot already has**: a strong heuristic score (`score_moves`), and one-ply rules that take wins (strikes)
  and avoid immediate losses (hard threat).
- **What this paper adds**: the **principle of blending** a search value with an existing heuristic instead of
  replacing it. A search value improves a decent heuristic; it does not have to replace it.
- **Battlecode compatibility**: high. It is the cleanest way to add search without throwing away 60 tuned terms.
- **CPU cost**: none.
- **Memory cost**: none.
- **Implementation complexity**: low.
- **Expected strategic value**: medium. It lowers the risk of R1.
- **Recommended adaptation**: for the queen in contact, score = heuristic + α·(search value scaled to the heuristic's
  range), α ≈ 0.3. Keep the hard LIFE terms outside the blend (proven losses stay losses).
- **Exact files/modules affected**: `decide.hpp` `score_moves` (queen branch); `config.hpp` (`CS_ALPHA`).
- **Priority**: use inside R1.

### 5.6 Samothrakis, Robles & Lucas 2010 — A UCT agent for Tron
- **Core idea**: alternating-move UCT for Tron (turned from simultaneous to alternating). Playouts forbid moves that
  trap the mover next step. When the players are walled off from each other it switches to a single-player "survival
  mode" whose reward is 1/(free cells).
- **Key findings**:
  - Results were noisy and depended on playout quality.
  - In the real competition MCTS entries ranked 25–200.
  - The winners used minimax with an excellent leaf evaluation.
  - The best UCT entry removed the random playouts and used a heuristic.
- **What my bot already has**:
  - the survival-mode idea (bunker, survival search);
  - the no-self-trap rule (ok1 / ok2, `survive_rec`, `loop_reachable`);
  - alternating turn order in reality: the queen moves first, then every enemy.
- **What this paper adds**: evidence that, for snake-type games under time limits, **deterministic minimax with a
  territory evaluation beats MCTS**; and the "separated → play for space" mode switch.
- **Battlecode compatibility**: high on mechanics (grid, bodies as walls, survival). Differences: we have partial
  vision, sprints, many agents, tails that move, pearls and a 500-round horizon.
- **CPU cost**: n/a.
- **Memory cost**: n/a.
- **Implementation complexity**: n/a.
- **Expected strategic value**: high as **design evidence** for R1's form (minimax, not MCTS).
- **Recommended adaptation**: R1 as two-ply max-min with a territory leaf. Add the "separated" check: when no modelled
  enemy can reach her region, R1 is skipped and the existing room / goal scoring decides.
- **Exact files/modules affected**: `contact.hpp`, `decide.hpp`.
- **Priority**: high (as the basis for R1).

### 5.7 Perick, St-Pierre, Maes & Ernst 2012 — Selection strategies in MCTS for Tron
- **Core idea**: simultaneous-move MCTS on joint moves, comparing 12 bandit selection rules at 100 ms per move.
- **Key findings**:
  - UCB1-Tuned is best.
  - Deterministic rules beat stochastic ones when there are few simulations.
  - A simple "no self-entrapment next step" playout rule wins 93% against random playouts.
  - Tuned rules explore more because Tron's branching is low.
  - The 2010 competition winner used alpha-beta with a "tree of chambers" evaluation.
- **What my bot already has**: the no-self-entrapment rule in every move check.
- **What this paper adds**: if a bandit or tree is ever used, choose **UCB1-Tuned**, deterministic, with a
  domain-knowledge playout. It also shows again that chamber (articulation) analysis drives strong Tron evaluations;
  we have the static version in topology.
- **Battlecode compatibility**: medium. Simultaneous moves don't apply; our order is sequential and known.
- **CPU cost**: n/a.
- **Memory cost**: n/a.
- **Implementation complexity**: n/a.
- **Expected strategic value**: low directly; it guides the optional R1 phase-2 variant.
- **Recommended adaptation**: if R1's deterministic version proves too shallow, phase 2 is UCB1-Tuned best-first
  search with heuristic leaves, not random playouts.
- **Exact files/modules affected**: `contact.hpp` (phase 2 only).
- **Priority**: low; optional.

### 5.8 Du, Gemp, Wu & Wu 2023 — AlphaSnake
- **Core idea**: AlphaZero-style policy iteration (MCTS plus a learned value/policy network) for single-player Snake.
  Forced moves are skipped; 200-state search per move.
- **What my bot already has**: forced-move shortcuts (single legal move). No learning.
- **What this paper adds**: learned evaluation. Main lesson: plain tree search with 10,000 states (no network) scored
  59 vs 98 for the trained agent, so learned guidance matters more than search size.
- **Battlecode compatibility**: low. No training infrastructure, a 48 MB wasm sandbox, and unseen maps (overfitting
  risk). Single-player only.
- **CPU cost**: inference of a small network is feasible in principle; training is the blocker.
- **Memory cost**: a model of a few hundred KB would fit.
- **Implementation complexity**: very high (self-play pipeline).
- **Expected strategic value**: low within the competition's time frame.
- **Recommended adaptation**: none. One idea worth keeping: **skip forced plies** in R1. If the queen has one legal
  non-losing move, don't branch on it.
- **Exact files/modules affected**: `contact.hpp` (forced-ply shortcut).
- **Priority**: do not implement (except the shortcut).

### 5.9 Silver & Veness 2010 — POMCP (Monte-Carlo planning in large POMDPs)
- **Core idea**: UCT over histories, plus an unweighted particle filter for the belief state: keep sampled states
  consistent with what has been observed, and add particles back with local changes. Domain knowledge enters as
  "preferred actions".
- **Key findings**:
  - Needs a black-box simulator, including a model of hidden agents (PocMan's ghost policy was known).
  - In Battleship the tree added little over flat rollouts because action values were close and noisy.
  - Gains appear from about 10³ simulations upward.
- **What my bot already has**: deterministic beliefs. Reports aged ≤ 3 with a growing reach, length memory, spawn
  prediction, death inference.
- **What this paper adds**: **a belief that learns from what we don't see.** A guessed enemy position falls inside our
  current view with no enemy there → discard that guess. Our current model can't express "she can't be there, I'm
  looking at that spot".
- **Battlecode compatibility**: the belief half fits well. The planning half doesn't: unknown opponent policy, many
  agents, small budget.
- **CPU cost**: (estimated) 20 enemies × 6 particles × 1 step per round ≈ 50k points per round. Negligible.
- **Memory cost**: < 10 KB.
- **Implementation complexity**: low-medium.
- **Expected strategic value**: medium-high, for unseen long strikers (the biggest class of queen deaths in the
  diagnosis) and for hunters guessing where their queen is.
- **Recommended adaptation**: **R2**, "enemy position guesses", replacing the growing circle for reports aged 1–8.
- **Exact files/modules affected**: `world.hpp` (struct `EnemyHyp`), `perceive.hpp` (advance and discard after
  `record_enemies`), `fields.hpp` (`compute_queen_threat` reads the guesses), `situation.hpp` / `commander.hpp`
  (`enemy_queen_target`).
- **Priority**: high.

### 5.10 Cowling, Powley & Whitehouse 2012 — Information Set MCTS
- **Core idea**: one tree over information sets; each iteration samples one determinization (a guess of the hidden
  state).
- **Two weaknesses of plain determinization it addresses**:
  - strategy fusion: planning as if you'll later know what you won't;
  - plus splitting the budget across separate trees.
- **What it does not address**: nonlocality, i.e. opponents steering play away from sampled states. That needs
  opponent modelling.
- **Key findings**:
  - It beats determinized UCT in a complex board game, Lord of the Rings: The Confrontation.
  - The version that assumes the opponent sees everything becomes so pessimistic that it plays randomly.
  - The experiments use 10⁴ iterations per decision.
- **What my bot already has**: one determinization (the reports), used deterministically.
- **What this paper adds**: averaging a decision over a few sampled hidden states. And a warning against fully
  pessimistic modelling: it is our "union reach balloons" failure (Q3) in another form.
- **Battlecode compatibility**: low for the full algorithm, which is far too expensive. The determinization idea is
  usable at depth 2, where strategy fusion barely matters.
- **CPU cost**: full ISMCTS is out of budget. Two to four determinizations in R1 multiply R1's cost by 2–4.
- **Memory cost**: small.
- **Implementation complexity**: high (full); low (determinizations in R1).
- **Expected strategic value**: medium only through R2 + R1.
- **Recommended adaptation**: R1 evaluates against the 2–4 most likely enemy-position guesses from R2 and takes the
  worse case for kills and the average for territory.
- **Exact files/modules affected**: `contact.hpp`.
- **Priority**: low; a later extension of R1 + R2.

### 5.11 Skrynnik, Andreychuk, Yakovlev & Panov 2024 — Decentralized MCTS for partially observable multi-agent pathfinding (MATS-LP)
- **Core idea**: each agent builds its own local model from what it sees and runs MCTS with a learned policy and value
  (CostTracer).
- **Search structure**: joint actions only for the **nearest K = 3 agents**; everyone else takes its single most
  likely action. 250 expansions per decision; no communication.
- **Ablations**:
  - planning only for yourself gets worse as density rises;
  - a random policy instead of the learned one is the worst variant.
- **What my bot already has**: decentralized decisions with no shared memory. Teammates are timed bodies plus an
  adjacency penalty; no prediction of teammates' or enemies' next moves.
- **What this paper adds**: two cheap structural ideas, and both work without MCTS:
  - plan jointly only with the few nearest agents;
  - predict everyone else with one greedy move.
- **Battlecode compatibility**: high for the structure; low for the learned policy (no training).
- **CPU cost**: as R1 for the K-agent part; greedy prediction is O(1) per agent.
- **Memory cost**: none.
- **Implementation complexity**: low.
- **Expected strategic value**: medium. It shapes R1's enemy model and R5 (teammate intent).
- **Recommended adaptation**:
  - R1 models the 2–3 nearest enemies' replies and moves the rest one greedy step (or leaves them in place);
  - R5 predicts each nearby higher-id teammate's next head cell from its claimed target, or straight ahead.
- **Exact files/modules affected**: `contact.hpp`, `tactics.hpp` (`mate_next_cell`), `decide.hpp` `score_moves`.
- **Priority**: medium.

### 5.12 Sharon, Stern, Felner & Sturtevant 2015 — Conflict-Based Search (CBS)
- **Core idea**: optimal multi-agent pathfinding with two levels. Find paths agent by agent, detect conflicts, branch
  on constraints. MA-CBS merges agents that keep conflicting.
- **Key findings**:
  - Strong where **conflicts concentrate in bottlenecks and corridors**; weak in open space.
  - The related sub-optimal methods (HCA*, WHCA*) plan agents one at a time in a fixed priority order with
    reservations, and can deadlock in bottlenecks.
- **What my bot already has**: greedy assignment; `PORTAL_ROW` (lower id first through a portal); `CORRIDOR_GUARD`
  (keep out of the queen's corridor).
- **What this paper adds**: (a) conflicts are worth resolving only in bottlenecks, which our topology already
  identifies (bridges); (b) a fixed priority order is a sound tie-break, and ours is free: the id order is the turn
  order.
- **Battlecode compatibility**: optimal CBS is impossible (no shared state, no full knowledge, 4 messages per turn).
  The "priority + bottleneck" pattern fits perfectly.
- **CPU cost**: negligible for R4.
- **Memory cost**: none.
- **Implementation complexity**: low.
- **Expected strategic value**: low-medium, larger on maze-like maps. It must first be confirmed by measurement (how
  many deaths are teammate clashes in corridors).
- **Recommended adaptation**: **R4**, "corridor right-of-way": generalise PORTAL_ROW and CORRIDOR_GUARD to every 1-wide
  bridge chain (topology). The lower id, or the dragon already inside, has the right of way; the other waits at the
  mouth or takes another route.
- **Exact files/modules affected**: `topology.hpp` (corridor id and mouths per bridge chain), `decide.hpp`
  `score_moves` (one rule replacing the two special cases).
- **Priority**: medium (measurement first).

### 5.13 Harabor & Grastien 2011 — Jump Point Search
- **Core idea**: prune symmetric paths in A* on uniform-cost grids (8-connected); about 10× faster expansions,
  optimal, no preprocessing.
- **What my bot already has**: BFS distance fields (single and multi-source), not point-to-point A*.
- **What this paper adds**: faster point-to-point search.
- **Battlecode compatibility**: low. We need distance fields to everything, with portals, timed bodies, unknown edges
  and wrap-around. JPS is designed for 8-connected point-to-point A*, and its symmetry pruning breaks with timed
  occupancy. A full BFS costs about 0.3M points (1/300 of the budget), so speed is not the problem.
- **CPU cost**: n/a.
- **Memory cost**: n/a.
- **Implementation complexity**: medium.
- **Expected strategic value**: none.
- **Recommended adaptation**: none. This matches the earlier "rejected by analysis" entry for adversarial A*.
- **Exact files/modules affected**: none.
- **Priority**: do not implement.

---

## 6. Capability gap analysis

### Current bot capability matrix

| Capability | Existing implementation | Strength | Main limitation | Relevant papers |
|---|---|---|---|---|
| Legal-move / one-step safety | `analyse_moves` (timed bodies, flood, ok1/ok2), B1b | exact one ply | none important | — |
| Self-trap avoidance | ok2, survival DFS (queen), pocket, dead-end, `topo_trap`, `loop_reachable` | strong on static traps | moving enemies that close a corridor are not modelled | Samothrakis, Perick |
| Enemy strike threat (1 turn) | `qThreat` from reach BFS (free + paid steps), attacker count, margin | exact (queens move first) | none | — |
| Enemy threat beyond 1 turn | `qThreat2` (two-turn union), `look2_safe_cells`, pursuit / corner penalties | cheap | worst-case union is too pessimistic and doesn't distinguish who is chasing her | Tron, Lanctot, MCTS-Solver |
| Unseen enemies | reports ≤ 3 rounds with growing reach; length memory | simple | expires too fast; growing longer balloons (Q3) | **POMCP**, ISMCTS |
| Opponent behaviour model | none (all enemies may go anywhere) | — | one fixed caution level for every opponent | ISMCTS (nonlocality), POMCP |
| Multi-move plans | sprint DFS (≤ 3 steps), dives (≤ 12), strike BFS | exact in their domain | single-agent: no enemy replies | Tron, Kocsis |
| Territory | Voronoi for goals (pearls, explore, camps) | good macro use | not used to judge moves in local fights | **Tron** |
| Static topology | Tarjan bridges, loop regions, branches | strong | bodies (dynamic chambers) not included | Tron (chambers) |
| Pearl assignment | greedy Manhattan matching + claims | measured best of the variants | views differ per dragon | CBS (negative), MATS-LP |
| Prey / pack assignment | greedy matching with ranks | works | depends on the guessed position of their queen | POMCP (beliefs) |
| Teammate conflicts | adjacency penalty, PORTAL_ROW, CORRIDOR_GUARD | covers portals and the queen | ad hoc; corridors in general not covered | CBS / HCA*, MATS-LP |
| Fleet-wide facts | permanent bits on STATUS (Q1) | reliable | only for facts that never expire | — |
| Decision effort control | node caps (survival 20k, dive 4k, pocket 3k) | safe CPU | effort not aimed at hard decisions | Chaslot |

**Central question: what important decision-making capability does the bot not have?**
**Reasoning about how the situation develops over the next 2–4 turns when enemies react.** All existing lookahead is
one of three kinds:
- single-agent (survival, dives, sprints);
- worst-case unions (`qThreat2`);
- static (topology).

So the queen's most important decisions, made under contact, rest on fixed penalty weights. The second missing
capability is **using what we don't see**: an enemy absent from cells in view tells us where it isn't.

---

## 7. MCTS analysis

### 7.1 Technique by technique: "What decision in my current bot would this improve?"

| Technique | Decision it could improve | Verdict | Why |
|---|---|---|---|
| Vanilla UCT | queen escape with several enemies | **Do NOT use MCTS here** | Random playouts are weak in snake games; at our budget (~300–1000 playouts) it is noisy; MCTS entries ranked 25–200 in the Tron competition. |
| UCB1-Tuned | the same, if a tree is ever built | **Only as phase 2 of R1** | Best selection rule in the Tron study, but only worth having once a tree exists. |
| RAVE | none | **Do NOT use** | Assumes a move's value is independent of when it's played; false here. |
| Progressive widening / unpruning | enemy reply sets in R1 | **Use the idea (not MCTS)** | Keep the top-k threatening reply cells; widen when budget allows. |
| Heuristic-guided MCTS (heuristic leaves, no random playouts) | queen escape, if depth 2 is not enough | **Only in specific situations** | The best Tron UCT entry was this design. Phase 2 of R1, only if phase 1 shows real depth-3 cases. |
| Implicit minimax backups | blending the search value with the existing score | **Use the idea** | α ≈ 0.15–0.4 blend; keeps 60 tuned terms useful. |
| Information Set MCTS | queen escape when enemy positions are uncertain | **Do NOT use MCTS here** | 10⁴ iterations; use 2–4 determinizations from R2 inside R1 instead. |
| POMCP | unseen enemies | **Do NOT use the planner; use its belief update (R2)** | The tree needs an enemy model and 10³ or more simulations. |
| Decentralized MCTS (MATS-LP) | teammate conflicts, packs | **Do NOT use MCTS; use its structure** | K-nearest joint replies plus greedy prediction for everyone else, without the tree. |

### 7.2 Decision by decision

| Decision | Current mechanism | Verdict |
|---|---|---|
| Choosing between several apparently safe moves | additive score | **Do NOT use MCTS here.** With no enemy in contact, the safety checks (ok1 / ok2, survival search, topology) already cover it; out-of-contact queen deaths are traps, and those are R1's job only when enemies close the exits. |
| Giving up immediate pearl value for long-term territory | Voronoi goals, matching | **Do NOT.** This is macro planning across ~60 dragons; no tree fits it. |
| Entering a contested region | Voronoi margin, conf ×0.35 | **Do NOT.** Use R2 (who is likely there) plus the existing margin. |
| Anticipating an enemy's likely response | none | **Search, but deterministic max-min (R1)**, queen only, in contact. |
| Avoiding traps beyond 1–2 steps | survival DFS (enemies static), topology | **Queen: R1** (enemies cutting exits). Others: topology is enough. |
| Attacking / intercepting | exact strike BFS | **Do NOT.** A one-turn kill is exact already. |
| Chasing their queen | prey matching, cork, BOX_BONUS | **Do NOT use MCTS.** Optional R7: the R1 engine with roles swapped. |
| Queen escape planning | reflexes plus score | **Use search (R1)**; heuristic-guided UCT only as phase 2. |
| Whether a sprint sequence is strategically good | sprint DFS value | **Do NOT use MCTS.** Feed sprint endpoints into R1 as queen actions. |
| Whether to split | rules (insurance, trap, growth) | **Do NOT use MCTS.** Add "insurance split" as one queen action in R1. |
| Coordinating several nearby dragons | implicit matching | **Do NOT.** Sonar can't carry a joint plan. |
| Exploration vs exploitation | explore / camp / zone choosers | **Do NOT.** The spawn model already estimates yields. |
| Reasoning about uncertain enemy positions | aged reports | **R2 (particles), no tree.** |

**Overall**: MCTS would materially improve the bot **only in one situation**: the queen in contact with 2+ enemies or
with ≤ 1 safe move. Even there, the papers' evidence favours deterministic two-ply max-min with a strong leaf
evaluation at our budget.

---

## 8. Partial-observability analysis

| | A. Current deterministic model | B. Light sampled enemy guesses (R2) | C. Full belief / information-set MCTS |
|---|---|---|---|
| What it is | reports ≤ 3 rounds old; reach grows with age; length memory | per enemy, K = 4–8 guessed positions moved each round by simple policies; guesses inside current view are discarded | particle filter + ISMCTS / POMCP tree per decision |
| Strategic benefit | baseline | covers enemies seen 1–8 rounds ago without ballooning; uses what we don't see; better hunt targets | in theory best; in practice limited by the unknown enemy policy |
| CPU (estimated) | ~0.1M points per turn (reach BFS) | +0.05M per round (moves) + reach BFS from at most K·(enemies near the queen) cells, ≈ +0.5–2M for the queen | 20M+ per decision for useful depth; over budget for routine use |
| Memory | trivial | < 10 KB | 1–5 MB per tree |
| Implementation | exists | ~150–250 lines | 1500+ lines plus a simulator |
| Robustness on unseen maps | good (no map assumptions) | good if the policies are generic (toward our queen, straight on, toward pearls) | poor: depends on the simulator's fidelity |

**Smallest approach that captures most of the benefit: B**, enemy position guesses. Rules that keep it from repeating
Q3's failure:
1. Guesses that land in a visible cell without that enemy are **discarded**: what we see shrinks the belief.
   Q3's circle could only grow.
2. Threat from guesses is weighted by how many guesses survive, and a guess older than ~8 rounds, or discarded by
   view, stops counting.
3. Only enemies with length ≥ 4 are tracked (the long-strike threat); short ones keep today's 3-round rule.
4. Phase 1 only **logs** guess accuracy (how far the enemy reappeared from the nearest surviving guess) before the
   queen reads it.

**No global probability distribution**: per-enemy guess sets only.

---

## 9. Tron / Snake analysis

| Dimension | Tron (papers) | Snake (AlphaSnake) | This game | Consequence |
|---|---|---|---|---|
| Information | full | full | 7×7 view + sonar | beliefs needed (R2) |
| Move timing | simultaneous | single player | sequential by id; queens first; every enemy replies once between the queen's turns | alternating max-min is the right model (R1) |
| Movement | 1 cell, constant speed | 1 cell | 1 cell + free steps ⌈L/4⌉ + paid steps (lose a segment each) | reply sets = reach sets; widening needed |
| Agents | 1 vs 1 | 1 | ~60 vs ~60 | model only the K nearest (MATS-LP) |
| Obstacles | trails that only grow | own body | kelp + bodies whose tails free over time + portals | timed occupancy (exists) |
| Territory | decisive (Voronoi / chamber evaluation) | — | decides local fights and the economy | **missing in move evaluation** (R1 leaf, R6) |
| Collisions | crash = loss; same cell = draw | self = loss | head-on kills both whatever the length; entering a body kills the mover | a queen in any enemy's reach is a proven loss (MCTS-Solver) |
| Objective | outlast the opponent | fill the board | survive (queen) + length at r500 + elimination | queen safety is the dominant term |
| Opponent modelling | min-max | none | unknown, varies a lot (passive locally, kamikaze on the ladder) | R3 aggression estimate |
| Horizon | ~75 moves | Θ(n³) | 500 rounds | only short-horizon search pays |

**Tron ideas that are missing:**
1. **Territory as the evaluation of a local fight.** Cells I reach before the enemy, around the contact point. We use
   Voronoi only to pick goals. → R1 leaf evaluation, R6.
2. **Opponent trapping with dynamic chambers.** Our chambers are static (kelp); bodies, including enemy heads that will
   move, aren't part of them. → R1 (the queen's room counted after enemy replies); R7 (boxing their queen).
3. **Mobility preservation as a search result, not a penalty.** `CORNER_PEN`, `ALL_EXITS_PEN` and `QUEEN_MOBILITY`
   are fixed penalties; in R1, mobility falls out of the max-min value.
4. **"Separated → survival mode" switch.** Partly there (bunker, survival search); R1 adds a clean trigger: if no
   modelled enemy can reach her region within 3 turns, skip the search.
5. **Move-space reduction.** Sprints already do it; R1 applies it to enemy replies.
6. **Search depth.** Tron winners searched deep with alpha-beta. Our branching (enemy reach sets) and partial view
   limit useful depth to 2 queen plies; 3 only with 4 queen moves × 3 replies.
7. **Deterministic over stochastic** (Perick): deterministic search throughout.

**Already present**: no self-entrapment, survival mode, static chamber analysis (bridges, loop regions), Voronoi.

---

## 10. Multi-agent coordination analysis

**Where "everyone computes the same assignment" breaks down (measured or observed in this project):**
1. **Different views.** Each dragon's `fleet` table holds a different subset of teammates: non-leader statuses aren't
   relayed, and rays stop at the first dragon or kelp. Matching is consistent only between dragons that see each
   other. MATCH_PATHS and MATCH_SWAP made this worse: precise costs from inconsistent inputs crowded dragons.
2. **Role elections.** Median 9 giants at once (measured). Claims don't spread (6d), gossip that expires doesn't
   converge (6d''), and stricter local rules cost Portals games.
3. **Pack prey.** The position guess of their queen differs per dragon.

**What can actually be built with sonar:**
- (a) **Priority by id with no messages.** The turn order is the id order, so "lower id has right of way" is
  consistent without any communication. This is the decentralized form of HCA* priority planning.
- (b) **Existing claims** (`T_CLAIM`: cell + ETA) give a teammate's intended target.
- (c) **Permanent bits on STATUS** are the only reliable fleet-wide channel.

**Assessment of each research idea:**

| Idea | Can it run under sonar? | Value | Verdict |
|---|---|---|---|
| CBS / MA-CBS (optimal) | no (needs a shared plan and full state) | — | do not build |
| HCA* / WHCA* (priority + reservations) | priority yes (the id order); fleet-wide reservations no | corridor conflicts | **R4**: priority only, locally, for bridge corridors |
| Decentralized MCTS (MATS-LP) | the structure yes; the learned policy no | head-ons with teammates, corridor clashes | **R5**: predict nearby higher-id teammates' next cell (greedy) |
| Pearl allocation | already greedy implicit | measured best | keep |
| Prey / packs | greedy implicit | depends on the queen guess | improve the guess with R2 (R10) |
| Guards / queen defence | slots + chokepoint posts | working | no change |
| Wall formation | id rank along the line | working | no change |
| Congestion avoidance | spread terms, crowd penalties | working | R4 / R5 cover corridors |
| Interception | exact strikes; STALKER_INTERCEPT OFF (−7 wins) | — | no change |

**R4 precondition (measurement):** the share of our dragons' "hit another dragon" deaths that are teammate clashes in
1-wide corridors. Use the replay tools (`doubledeath.py`, `trades.py`). Build R4 only if it is ≥ 5% of non-deliberate
deaths on maze-like maps.

---

## 11. CPU feasibility analysis

Budget: 100M points per dragon per turn. Current peak 49.7M (64×64), p50 28.1M. Safe extra headroom ≈ 35–40M **per
dragon**; a queen-only search uses only the queen's budget. Contact frequency (measured): the queen sees an enemy head
in **7.4%** of her turns (5,875 of 79,085). Of those turns:

| Queen state | Turns |
|---|---|
| No fully safe move | 896 |
| At most one candidate passes both safety checks | 1,464 |
| 2+ enemy heads within 6 | 1,562 |

| Proposal | Runs when | Branching / depth | Work per run (estimated) | Worst case | Memory | Floating point | Reuses | Incremental | Limited to |
|---|---|---|---|---|---|---|---|---|---|
| **R1 queen contact search** | queen, an enemy head within its reach + 3, and (≤ 1 safe candidate, or 2+ heads within 6, or best-vs-second score margin small) | queen actions ≤ 8 (4 steps + ≤ 4 sprint ends + insure split); 2–3 enemies × top-k 4–6 replies; depth 2 queen plies | depth 2: 8 × 6 × 8 = 384 leaves × ~35k points per leaf (radius-10 two-source BFS + loop check + 200-node survival) ≈ **13–15M** | 20M (node cap) | < 200 KB (preallocated stamps, body buffers) | blend only | flood, reach BFS, `simulate_steps`, topology, stamp arrays | no (per turn) | queen, ~2–7% of her turns |
| R1 depth 3 (optional) | the same, when depth 2 ties | 4 × 3 × 4 × 3 × 4 = 576 leaves | ~20M | 25M | same | same | same | no | queen, rare |
| **R2 enemy guesses** | every round, every dragon (cheap); the queen reads them | 20 enemies × K 6 | ~0.05M to move + 0.5–2M of extra reach BFS for the queen | 3M | < 10 KB | no | reach BFS, `vis` | yes (moved, not rebuilt) | tracking for all; threat for the queen |
| **R3 aggression estimate** | every round | counters | < 0.01M | — | bytes | ratio | `heads`, dangers | yes | all |
| R4 corridor right-of-way | candidate moves into a bridge chain | 1 lookup per candidate | < 0.01M (+ corridor labels at topology rebuild, ~0.2M) | — | N ints | no | topology | rebuilt with topology | all |
| R5 teammate next cell | teammates within 3, higher id | 1 step each | < 0.05M | — | bytes | no | claims, `distHead` | no | all |
| R6 local territory term | non-queen, enemy head within 4 | 4 candidates × radius-6 two-source BFS | ~0.2M | 0.5M | reuse | no | Voronoi BFS code | no | combat / hunters |
| R7 cork search | hunter sees their queen, no strike | R1 engine, roles swapped | ~5–10M | 15M | same | blend | R1 | no | hunters, rare |

Design rule: **cheap heuristic → detect uncertainty → deep search only then.** Chaslot's progressive strategies and
the node caps we already use support this pattern. R1 needs a hard node cap, falling back to the plain score, as the
survival search does.

---

## 12. Recommended hybrid architecture

```
PERSISTENT WORLD MODEL  (edges, portals, symmetry, pearls, reports)            [unchanged]
  + R2 ENEMY GUESSES (per-enemy positions; what we see discards guesses)        [new, world/perceive]
  + R3 OPPONENT AGGRESSION ESTIMATE  -> caution profile (two tested settings)   [new, situation]
        |
FIELDS  distHead, Voronoi, qThreat (exact one ply; for aged reports read from guesses), region value   [fields]
        |
HARD SAFETY RULES  legality, exact one-ply death, static traps (topology)      [unchanged: proven losses]
        |
REFLEXES  exact strikes, escape sprint, survival / insurance split             [unchanged]
        |
SELECTIVE CONTACT SEARCH (queen only, when uncertain)                          [new R1, contact.hpp]
   max-min over queen actions x replies of the 2-3 nearest enemies (top-k, from guesses when unseen)
   leaf = territory she reaches first (radius 10) + loop available + survival feasible; proven losses pruned
        |
STRATEGIC HEURISTICS  targets, roles, matching (+ R4 corridor priority, R5 teammate next cell)   [existing]
        |
SCORE = heuristic sum  +  alpha x search value (queen in contact only)         [blend, Lanctot]
        |
SPRINT EXTENSION (existing; for the queen in contact, sprint ends are R1 actions)  -> ACTION
```

**What stays:** all of perception, topology, BFS / flood, reflexes, the coordination rules, the band tagging.
**What the new parts replace over time** (only after they prove themselves, switches first):
- the look-2 term;
- the pursuit / corner / all-exits penalties;
- crowd flight (`choose_flee` / QUEEN_CROWD) in contact states;
- the aged-report growth in `compute_queen_threat`.

---

## 13. Top 10 improvements (ranked by expected ladder rating impact)

| Rank | Change | Current weakness addressed | Paper | Expected rating impact | CPU impact | Implementation difficulty | Risk |
|---|---|---|---|---|---|---|---|
| 1 | **R1 Queen contact search** (two-ply max-min, territory + loop leaf, blended α≈0.3, selective) | additive room-vs-threat weights; traps by enemies cutting exits (19 of 36 queen deaths are not head-ons; "cornered by 3–5 heads") | Samothrakis, Perick, Lanctot, Winands (Solver), Chaslot, MATS-LP | **High** (queen deaths are the main loss cause on the ladder) | +13–20M on ~2–7% of queen turns | Medium-high (~350 lines) | Medium |
| 2 | **R2 Enemy position guesses** (discard by view) feeding the queen's danger map for reports aged 1–8 | blind to long strikers from out of view (the largest diagnosed class) without Q3's balloon | POMCP, ISMCTS | Medium-high | +0.5–2M (queen) | Medium (~200 lines) | Medium |
| 3 | **R3 In-game opponent aggression estimate** → picks between the current profile and a tested cautious profile (REP_MAX_AGE 6, stronger look-2) | one caution level vs very different opponents | ISMCTS (nonlocality needs opponent modelling), POMCP | Medium | ~0 | Low (~80 lines) | Low-medium (misclassification) |
| 4 | **R4 Corridor right-of-way** by id priority on bridge chains (generalises PORTAL_ROW + CORRIDOR_GUARD) | teammate clashes in 1-wide corridors | CBS / HCA* | Low-medium (maze maps) | ~0 | Low | Low |
| 5 | **R5 Teammate next-cell prediction** for higher-id neighbours (claims, else straight on) | head-ons with teammates and blocking handled by a flat adjacency penalty | MATS-LP | Low-medium | ~0 | Low | Low |
| 6 | **R6 Local territory term** for combat dragons and hunters near an enemy (replaces the confinement bonus) | local fights judged by ad hoc pin bonuses | Tron papers | Low-medium | +0.2M when in contact | Low-medium | Medium (touches tuned terms) |
| 7 | **R7 Cork / boxing search** for hunters with their queen in view (R1 engine, roles swapped) | BOX_BONUS heuristic; strikes only when exact | Tron, MCTS-Solver | Medium on the ladder (their queens are seen there); ~0 locally | +5–10M, rare | Low once R1 exists | Low-medium |
| 8 | **R8 Uncertainty trigger + node budget guard** (the framework for R1 / R7) | no way to spend effort on hard decisions | Chaslot | enabling | ~0 | Low | Low |
| 9 | **R9 Test harness: counterfactual queen-death replays + an aggressive sparring profile** | local suite saturated; caution tuned against passive bots | (Tron papers: compare against strong opponents) | enabling (decides 1–3) | — | Low-medium | none |
| 10 | **R10 Hunt targets from R2 guesses of their queen** | prey position guessed from one stale report / mirror spawn | POMCP | Low-medium | ~0 | Low after R2 | Low |

### Top 3 low-risk / high-return
1. **R3 opponent aggression estimate.** It switches between two configurations we have already tested. It goes
   straight at the "local vs ladder caution" conflict.
2. **R4 corridor right-of-way.** It is one rule that replaces two special cases, and only acts in conflict
   situations. Measurement-gated.
3. **R5 teammate next-cell prediction.** Narrow, cheap, and replaces a flat penalty.

### Top 3 high-upside / high-risk
1. **R1 queen contact search.** The biggest structural gain; it touches the queen's most sensitive decisions.
2. **R2 enemy guesses feeding the queen's danger map.** Q3 showed that more caution can lose games.
3. **R7 cork search + R10 guess-based hunting.** Ladder-only upside; can't be validated locally (the cork never
   triggered in 204 games).

### Top 3 ideas that look impressive but should NOT be implemented
1. **General MCTS / UCT (or ISMCTS / POMCP planning) for every dragon or every turn.** Over budget for useful depth,
   random playouts are weak in snake games, the enemy policy is unknown, and the papers' own evidence (Tron
   competition) favours minimax plus evaluation.
2. **CBS / optimal multi-agent pathfinding or global optimal matching.** It needs a shared plan; sonar can't carry
   one, and "better" assignments were measured to crowd dragons (MATCH_PATHS, MATCH_SWAP).
3. **Learned policies (AlphaZero-style, MATS-LP's CostTracer) and RAVE / JPS.** No training pipeline and a risk of
   overfitting to unseen maps; RAVE's assumption fails here; JPS speeds up a search we don't need sped up.

---

## 14. Top 3 immediate changes (in order)
1. **R8 + R9 first (no behaviour change).** Add a QDIAG "contact episode" log: entry state, the queen's choice,
   survival over the next 10 rounds. Add a node-budget helper. Build the counterfactual run of the queen-death games
   (re-run the exact seeds with a switch on and compare those deaths). This makes R1–R3 measurable beyond the
   saturated 204-game total.
2. **R1 queen contact search (MVP)**: depth 2, 2 enemies, top-k 5 replies, α = 0.3, behind `CONTACT_SEARCH`,
   default OFF until it passes.
3. **R3 opponent aggression estimate**: logged first on all sparring bots to check that it separates vib2
   (aggressive) from fleet11 (passive), then wired to the caution profile.

---

## 15. Ideas not worth implementing
- Vanilla UCT / UCB1 / UCB-V / Thompson sampling / EXP3 as a general decision method (Perick: stochastic rules are
  weakest at small budgets; random playouts are weak).
- RAVE / AMAF (the assumption fails).
- Full ISMCTS / POMCP tree search (budget; pessimistic variants play randomly).
- CBS / MA-CBS / HCA* with fleet-wide reservations (no shared memory).
- JPS / A* speed-ups (BFS ≈ 0.3% of the budget).
- AlphaSnake / AlphaZero-style learning (no training infrastructure; wasm / 48 MB; overfitting risk).
- Deeper uniform alpha-beta for every dragon (no value outside contact: 92.6% of queen turns have no enemy in view).
- Any new fleet-wide agreement on facts that change (measured to fail; only permanent STATUS bits work).

---

## 16. Proposed experiments

| # | Experiment | Measures | Accept if |
|---|---|---|---|
| E1 | Contact-episode logging on v3.2.0 (204 games) | P(queen survives 10 rounds after entering contact), by entry state (heads, safe candidates) | baseline only |
| E2 | Counterfactual R1: re-run the 36 queen-death games with `CONTACT_SEARCH=1`; then the full suite on seeds 1–2 and 3–4 | deaths avoided / created; total W-L; queen deaths; protected maps on both seed sets; CPU p99 / max | queen deaths −15% or more over 408 games, protected losses ≤ +1 per seed set, CPU max < 85M |
| E3 | R2 in log-only mode | when an enemy reappears: distance to the nearest surviving guess vs today's circle radius; share of reappearances covered | guesses cover ≥ 80% with a mean area < 50% of the circle's |
| E4 | R2 feeding the danger map (queen) | same as E2, plus Portals specifically (Q3 lost 5 there) | as E2 |
| E5 | R3 detector accuracy | aggression score per opponent (vib2 vs swarm2 vs fleet11) by round 60 / 100 | separates vib2 from fleet11 in ≥ 90% of games |
| E6 | R3 wired: profile switch | suite vs each opponent; ladder A/B (two zips) | no local loss; ladder decides |
| E7 | R4 precondition | teammate-corridor clash deaths per game (replays) | ≥ 5% of non-deliberate deaths on maze-like maps → build R4 |
| E8 | R5 / R6 | suite on two seed sets | the standard rule on both seed sets |

**Acceptance rule update**, given the measured seed noise (v3.2.0: Portals 0 losses on seeds 1–2, 6 on seeds 3–4):
judge queen-safety changes on **both seed sets (408 games)**, plus the counterfactual death games, plus a ladder A/B.
Don't rely on one 204-game run.

---

## 17. Implementation roadmap

| Phase | Content | Switch | Gate |
|---|---|---|---|
| 0 | R8 node-budget helper + uncertainty trigger (computed, unused); R9 contact-episode logging; counterfactual runner | — | identical play (trace) |
| 1 | R1 MVP: depth 2, 2 enemies, top-k 5, territory radius 10 + loop + survival leaf, proven-loss pruning, forced-ply skip, α 0.3 | `CONTACT_SEARCH` | E2 |
| 2 | R3 detector (log) → profile switch | `AGGRO_ADAPT` | E5, E6 |
| 3 | R2 guesses: log-only → queen's danger map → R1 enemy set (2–4 determinizations) | `ENEMY_HYP` | E3, E4 |
| 4 | R4 (if E7), R5 | `CORRIDOR_ROW`, `MATE_PREDICT` | E8 |
| 5 | R6, R7, R10; optional R1 phase 2 (UCB1-Tuned with heuristic leaves) only if E1 / E2 show depth-3 cases dominate the remaining deaths | separate switches | E8 / ladder |
| 6 | Clean-up: once a replacement has passed on both seed sets and the ladder, remove what it superseded (section 18) | — | trace / suite |

---

## 18. Exact files / functions affected

| Change | Files | Functions / structures |
|---|---|---|
| R1 | **new `fleet/contact.hpp`** (included after `act.hpp`, before `decide.hpp`); `decide.hpp`; `tactics.hpp`; `config.hpp`; `main.cpp` (QDIAG) | `contact_search()`, `cs_leaf_eval()`, `cs_enemy_replies()` (top-k ordering), `cs_queen_actions()` (from `cands` + `sprint_dfs` endpoints + insure split); new `Cand::searchVal`; blend in `score_moves` (queen branch); called after `read_contact` |
| R2 | `world.hpp`, `perceive.hpp`, `fields.hpp`, `config.hpp` | `struct EnemyHyp {id, cell, len, policy, born}`; `advance_hypotheses()` + `prune_by_view()` after `record_enemies()`; `compute_queen_threat()` reads guesses instead of `e.cell` with growing `f*(age+1)` |
| R3 | `perceive.hpp`, `situation.hpp`, `fields.hpp`, `decide.hpp`, `config.hpp` | counters in `record_enemies()` / `detect_teammate_deaths()` (enemy heads closing in on ours within reach; head-on deaths with cause 1); `sit.aggro` in `compute_fleet_state()`; `rep_max_age()`, `look2_pen()` turn into functions of the profile |
| R4 | `topology.hpp`, `decide.hpp` | corridor id / mouth per bridge chain in `topo_build()`; one rule in `score_moves` replacing the `PORTAL_ROW_PEN` block and `CORRIDOR_GUARD` |
| R5 | `perceive.hpp` (remember claims per teammate), `tactics.hpp`, `decide.hpp` | `mate_next_cell(id)`; replaces the `TEAM_ADJ_PEN` adjacency term |
| R6 | `fields.hpp`, `decide.hpp` | `local_territory(cell, enemyCell, radius)`; replaces the confinement `BAND_STYLE` block (3500 × mult, 1500 × area share) |
| R7 | `contact.hpp`, `decide.hpp` | `cork_search()`; used by `tg_cork` / the `BOX_BONUS` block |
| R8 | `core.hpp`, `decide.hpp` | `budget_ok()`; `decision_margin()` from the candidate bands |
| R9 | `main.cpp` (QDIAG), `tools/testing/` | contact-episode log line; counterfactual runner script |
| R10 | `situation.hpp`, `commander.hpp` | `enemy_queen_target()` reads R2's queen guesses |

**What becomes unnecessary as a consequence (after validation):**
- **R1 (queen in contact)**:
  - `look2_safe_cells` / `qThreat2` / `LOOK2_*` terms;
  - the pursuit penalties `CORNER_PEN`, `ALL_EXITS_PEN`, `QUEEN_MOBILITY`;
  - `choose_flee` / `QUEEN_CROWD` in contact;
  - the OFF switches `QUEEN_PAID_ESCAPE`, `QUEEN_EVADE`, `STALKER_INTERCEPT` (already rejected) can be deleted.
- **R2**: the age-growth branch in `compute_queen_threat`; `REP_MAX_AGE` as a constant.
- **R3**: the need to compromise one global caution setting.
- **R4**: `PORTAL_ROW_PEN` and `CORRIDOR_GUARD` become one rule.
- **R5**: `TEAM_ADJ_PEN`.
- **R6**: the confinement STYLE terms. **R7**: `BOX_BONUS`.

The ~45 rejected OFF switches can stay, as you decided. They cost nothing at runtime.

---

## 19. References
1. L. Kocsis, C. Szepesvári. Bandit based Monte-Carlo planning. ECML 2006. (`Kocsis+Szepesvari_2006.pdf`)
2. S. Gelly, D. Silver. Monte-Carlo tree search and rapid action value estimation in computer Go. Artificial
   Intelligence 175, 2011. (`1-s2.0-S000437021100052X-main.pdf`)
3. G. Chaslot, M. Winands, J. Uiterwijk, H. van den Herik, B. Bouzy. Progressive strategies for Monte-Carlo tree
   search. New Math. and Nat. Computation 4(3), 2008. (`chaslot-et-al-2011-...pdf`)
4. M. Winands, Y. Björnsson, J.-T. Saito. Monte-Carlo tree search solver. CG 2008. (`Monte-Carlo_Tree_Search_Solver.pdf`)
5. M. Lanctot, M. Winands, T. Pepels, N. Sturtevant. Monte Carlo tree search with heuristic evaluations using implicit
   minimax backups. IEEE CIG 2014. (`Monte_Carlo_Tree_Search_with_heuristic_evaluations_...pdf`)
6. S. Samothrakis, D. Robles, S. Lucas. A UCT agent for Tron: initial investigations. IEEE CIG 2010.
   (`A_UCT_agent_for_Tron_Initial_investigations.pdf`)
7. P. Perick, D. St-Pierre, F. Maes, D. Ernst. Comparison of different selection strategies in Monte-Carlo tree
   search for the game of Tron. IEEE CIG 2012. (`tron2012.pdf`)
8. K. Du, I. Gemp, Y. Wu, Y. Wu. AlphaSnake: policy iteration on a nondeterministic NP-hard Markov decision process.
   AAAI 2023 (student abstract). (`07443-AAAI23.DuK-SA.pdf`)
9. D. Silver, J. Veness. Monte-Carlo planning in large POMDPs. NIPS 2010.
   (`NIPS-2010-monte-carlo-planning-in-large-pomdps-Paper.pdf`)
10. P. Cowling, E. Powley, D. Whitehouse. Information set Monte Carlo tree search. IEEE TCIAIG 4(2), 2012.
    (`CowlingPowleyWhitehouse2012.pdf`)
11. A. Skrynnik, A. Andreychuk, K. Yakovlev, A. Panov. Decentralized Monte Carlo tree search for partially observable
    multi-agent pathfinding. AAAI 2024. (`13274-AAAI24.SkrynnikA.pdf`)
12. G. Sharon, R. Stern, A. Felner, N. Sturtevant. Conflict-based search for optimal multi-agent pathfinding.
    Artificial Intelligence 219, 2015. (`1-s2.0-S0004370214001386-main.pdf`)
13. D. Harabor, A. Grastien. Online graph pruning for pathfinding on grid maps (jump point search). AAAI 2011.
    (`harabor-grastien-aaai11.pdf`)
- Cited inside 6 and 7, not read directly: A. Sloane's 2010 Google AI Challenge (Tron) post-mortem (alpha-beta with a
  "tree of chambers" evaluation).
- Project data: `Changes_Tracker.md`, `results/sV320.txt`, `results/sV320b_s3.txt`, the QDIAG diagnostic run
  (204 games, 2026-10-07), `Vibing++` ladder analysis (memory notes).

---

## Final answers

1. **Biggest algorithmic weakness**: no lookahead with enemy replies. All lookahead beyond one ply is single-agent
   (survival, dives, sprints), a worst-case union (`qThreat2`) or static (topology). So the queen's decisions in
   contact rest on a fixed sum of penalty weights.
2. **Biggest strategic weakness**: one fixed caution / aggression setting for every opponent. Local tests reward
   boldness (Portals); the ladder punishes it (kamikaze sprints caused 11 of 16 queen deaths in the Vibing++ analysis).
   Second: the opening lags ladder leaders in rounds 1–50, which local opponents don't reproduce.
3. **Biggest weakness caused by partial observability**: enemies that left view are forgotten after 3 rounds or
   modelled by a circle that grows with age. Long strikers from outside the 7×7 view are the largest diagnosed class
   of queen deaths, and "nobody saw anything there" is never used as evidence.
4. **Biggest weakness in enemy modelling**: no intent at all. Every enemy is assumed able to go anywhere it can reach,
   and nothing records how this particular opponent actually behaves.
5. **Biggest weakness in multi-dragon coordination**: agreement depends on the fragmented sonar network. Only
   permanent facts spread fleet-wide, so roles (giants: median 9 at once) and assignments diverge between dragons.
   Corridor conflicts between teammates are handled only for portals and the queen's corridor.
6. **Would MCTS materially improve this bot?** **ONLY IN SPECIFIC SITUATIONS**: the queen in contact with enemies.
   Even there, the first choice is deterministic two-ply max-min, not MCTS.
7. **Where**: in `decide()` after `read_contact`, before `score_moves`, for the **queen only**. Triggered when an
   enemy head is within its reach + 3 and (≤ 1 safe candidate, or 2+ heads within 6, or a small best-vs-second score
   margin). Later, the same engine for hunters boxing a visible enemy queen (R7).
8. **Which variant**:
   - Phase 1: deterministic max-min with progressive-widening-style reply pruning, proven-loss pruning (MCTS-Solver)
     and implicit-minimax blending (α ≈ 0.3).
   - Phase 2, only if needed: UCB1-Tuned best-first search with heuristic leaves (no random playouts).
   - Never vanilla UCT, RAVE or full ISMCTS / POMCP.
9. **Realistic depth / budget**:
   - Depth 2 queen plies (queen → enemy replies → queen → leaf): ≤ 8 queen actions × 2–3 enemies × top 4–6 replies,
     ≈ 400 leaves, ≈ 13–15M points, hard cap 20M.
   - Depth 3 only with 4 actions × 3 replies (≈ 576 leaves, ≈ 20M).
   - If phase 2 is ever built: about 300–600 heuristic-leaf iterations.
10. **Every turn or selected conditions?** Selected conditions only: about 2–7% of queen turns (measured contact rate
    7.4%), never for other dragons except R7.
11. **State representation**:
    - the queen's body (head first, length) and the free / paid step budget;
    - for the 2–3 nearest enemies: head cell, estimated length, and (if unseen) 2–4 guesses from R2;
    - other dragons as timed bodies (`otherRel`, existing);
    - the edge graph (`cross` / `step_open`), topology loop regions, pearl cells within radius 10.

    Bodies are kept in preallocated buffers with stamp arrays; no heap allocation per node.
12. **Leaf evaluation (no random rollouts)**:
    - Terminal: the queen inside any modelled enemy's reach after her move → proven loss.
    - Otherwise a weighted sum of:
      - (a) cells she reaches strictly before every modelled enemy within radius 10 (Tron Voronoi);
      - (b) whether a free loop longer than her is reachable (`loop_reachable` / `topo_room`);
      - (c) a 200-node survival check;
      - (d) the number of free exits next turn;
      - (e) a small pearl / goal term.

    Scale the result to the heuristic's range and blend with α ≈ 0.3.
13. **Should enemy replies be modelled explicitly?** Yes, for the 2–3 nearest enemies: top-k threatening placements
    within their reach, worst case for kills and the minimizing reply for territory. Everyone else moves one greedy
    step or stays (MATS-LP).
14. **Should uncertainty be modelled explicitly?** Yes, but small: per-enemy guess sets that are moved each round and
    discarded by what we see (R2). R1 evaluates against 2–4 of them. No global probability distribution and no belief
    tree.
15. **Single highest-value change to build first**: **R1, the queen contact search**, behind `CONTACT_SEARCH`, after the
    phase-0 logging so its effect on contact survival can be measured directly.
16. **The next four**:
    1. **R3** opponent aggression estimate → caution profile.
    2. **R2** enemy guesses: log, then the danger map, then R1's enemy set.
    3. **R4** corridor right-of-way, if the clash measurement supports it.
    4. **R7** cork search for hunters (reuses R1), with **R10** guess-based hunt targets.
17. **What to remove or simplify as a consequence** (after each replacement passes on both seed sets and the ladder):
    - **R1**: the queen's look-2 term and `qThreat2` usage, the pursuit corner / all-exits / mobility penalties, and
      crowd flight in contact states; delete the rejected escape switches (`QUEEN_PAID_ESCAPE`, `QUEEN_EVADE`,
      `STALKER_INTERCEPT`).
    - **R2**: the age-growth reach for aged reports and the `REP_MAX_AGE` constant.
    - **R4**: merges `PORTAL_ROW_PEN` and `CORRIDOR_GUARD` into one rule.
    - **R5**: removes `TEAM_ADJ_PEN`.
    - **R6 / R7**: remove the confinement STYLE terms and `BOX_BONUS`.
