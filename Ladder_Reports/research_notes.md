# Research notes: papers and techniques for the UNSW Battlecode bot (2026-10-09)

Mapped to the gaps the ladder replays keep showing: (1) the economy gap against top teams (they eat ~2.5x and take
~half of our death drops), (2) our queen dying once the fleet is outnumbered or to long hunters striking from out of
view, (3) not knowing that our own unseen strike killed their queen.

| Source | Core idea | Where it fits our bot |
|---|---|---|
| Stanescu, Barriga, Buro, "Using Lanchester Attrition Laws for Combat Prediction in StarCraft" (AIIDE 2015); "Combat Models for RTS Games" (arXiv 1605.05305) | Square law: an army's fighting strength grows with the square of its size; predict who wins a fight from local counts and decide attack or retreat | Contest pearls, the middle and trades only where the *local* force ratio (our heads vs theirs within a radius) favours us; give way where it does not. Our data: even 1-for-1 head-on trades help the side with more dragons |
| Liu, Louis et al., influence maps + potential fields for StarCraft micro (CIG 2013/2014, CEC 2013); Critch & Churchill, sneak attacks with influence maps (CoG 2021) | A grid where each unit adds influence that decays with distance (enemies negative): a threat map for positioning | One fleet-wide map of our minus their influence: (a) pearls deep in their influence are worth less (our dragons die there and they eat the drops); (b) the queen rests where our influence is highest (escorted ground); (c) dying dragons are already counted |
| Weber, Mateas, Jhala, "A Particle Model for State Estimation in RTS Games" (AIIDE 2011); Southey et al. (HMMs); Hladky & Bulitko | Track units after they leave view with a motion model plus decaying occupancy; +10% for their bot | Done for their queen (EQ_BELIEF). Next: track every *long* enemy seen recently (their reach grows with age x speed), so the queen's safety field still counts a long hunter that just left view (Australia r463, Islands r446 deaths) |
| Battlesnake: "limiting search to players in the locality" (2019, alpha-beta + max^n, diamond flood fill); coreyja's paranoid minimax + flood-fill scoring | Search only the snakes near you; assume they play against you (paranoid); score by reachable space | A queen-only local search: her moves vs the 1-3 nearest enemy heads, 2-3 plies, scored by safety field + flood-fill space |
| Tron (Google AI Challenge 2010: a1k0n, Janzert): Voronoi territory, articulation points / 2-connectivity | Score positions by cells we reach first; split the board at articulation points to avoid sealing yourself into a chamber | Pocket and trap checks (in_pocket, POCKET_GUARD) are local versions; Tarjan biconnected components would give exact chambers for the queen's flee/rest target |
| Lisy et al., "Convergence of MCTS in Simultaneous Move Games" (NeurIPS 2013); Den Teuling (Tron MCTS); arXiv 2009.08807 | Serialising simultaneous moves biases search: opponent-moves-after-us = defensive, before-us = aggressive | Our threat maps assume enemies may move anywhere after us (defensive): right for the queen, too timid for farmers |
| Response threshold models for swarm foraging: Castello et al. (ARTM); Lee, Vaughan, Kim (IEEE Access 2020); Wu et al. (AAAI) | Each agent takes a job with probability s^2/(s^2+theta^2) (s = demand, theta = its own threshold); varied thresholds stop everyone switching at once; no messages needed | Replace hard cut-offs (escort at 5 vs 8 dragons, huddle at 4:1) with smooth demand-driven switching: demand = queen threat, theta varies by dragon id - fewer all-or-nothing swings like v5.1.32's escort |
| Bertsekas auction; Bai, Yan, Ge (TSMC 2022, range-limited robots); CMU grouped-task auction | Distributed assignment with local information; greedy/marginal-cost bounded within 2x optimal | Our exact Hungarian matching swung by map (Schooltime +33%, Stripes -40%) because it trusts guessed teammate positions; keep greedy + claims, or a sequential single-item auction over T_CLAIM |

## Ranked by expected value for the remaining time
1. Recent long-hunter tracking in the safety field (particle-model idea): cheap, targets late queen deaths.
2. Local Lanchester ratio for contesting pearls / trades: cheap, targets head-on losses and drop feeding.
3. Influence map (fleet-wide): moderate, targets drops eaten by them and the lone queen.
4. Response-threshold job switching: moderate, makes escort/huddle robust.
5. Queen local paranoid search: larger, CPU care needed.
