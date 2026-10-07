// CONFIG - every tunable switch, grouped by module. Override with -D or a #define before this
// header (tools/testing/suite.sh and trace.py prepend #defines to main.cpp). Every name must be unique:
// run tools/testing/check_switches.py. Switches marked OFF were tested and are kept for experiments.
#pragma once

// ====================================================================================================
// OPENING, FLEET SIZE AND SPLITS
// ====================================================================================================
#ifndef EARLY_SPLIT_ROUNDS
#define EARLY_SPLIT_ROUNDS 150 // "X": rounds of aggressive splitting (section 2)
#endif
#ifndef EARLY_SPLIT_MIN_LEN
#define EARLY_SPLIT_MIN_LEN 4 // split as soon as length reaches this (section 2)
#endif
#ifndef LEADER_COUNT
#define LEADER_COUNT 3 // leader dragons in the growth phase (section 11)
#endif
#ifndef PRESERVE_SPLIT_LEN
#define PRESERVE_SPLIT_LEN 8 // non-leaders split at this length to keep numbers
#endif
#ifndef SPLIT_DENSITY
#define SPLIT_DENSITY 32 // at most one dragon per this many tiles (small maps get crowded)
#endif
#ifndef TRANS_SHIFT
#define TRANS_SHIFT 100 // leader phase starts this much earlier than the section-12 table (tuned)
#endif
#ifndef EXPANSION
#define EXPANSION 1 // numbers first: everyone (queen included) splits from round 0 up to a denser cap
#endif
#ifndef EXP_DENSITY
#define EXP_DENSITY 12 // (only with DYN_ALLOC 0; DYN_DENSITY applies) expansion cap: one dragon per this many tiles (64 at most)
#endif
#ifndef EXP_END
#define EXP_END 350 // (only with DYN_ALLOC 0; growth ends at the switch) after this round the normal SPLIT_DENSITY cap applies again
#endif
#ifndef FAST_OPEN
#define FAST_OPEN 1 // 2.1: hunters split at EARLY_SPLIT_MIN_LEN too while the fleet is still expanding
#endif
#ifndef LANE_SPLIT
#define LANE_SPLIT 1 // 2.2: a dragon whose tail sits among spawn tiles splits there, the child inherits the lane
#endif
#ifndef LANE_TILES
#define LANE_TILES 3 // spawn tiles within 3 of the tail that make a lane
#endif
#ifndef LANE_DENSITY
#define LANE_DENSITY 8 // lane splits allowed up to one dragon per this many tiles
#endif
#ifndef RUNNERS
#define RUNNERS 1 // 2.5: explorer-role dragons on big maps stay length >= 5 (2 free steps a turn)
#endif
#ifndef RUNNER_MIN_CELLS
#define RUNNER_MIN_CELLS 1200
#endif
#ifndef RUNNER_KEEP
#define RUNNER_KEEP 5
#endif
#ifndef RUNNER_SPLIT_LEN
#define RUNNER_SPLIT_LEN 8
#endif
#ifndef SPLIT_ROOM
#define SPLIT_ROOM 0 // (v2.14.4: off - faster splitting) growth splits only where the child has room (no children born into pockets)
#endif
#ifndef DYN_ALLOC
#define DYN_ALLOC 1 // numbers first on every map until a switch round set by how many enemies we see, then length
#endif
#ifndef SW_LO
#define SW_LO 260 // switch round when we see (almost) no enemies...
#endif
#ifndef SW_HI
#define SW_HI 400 // ...and when we see SW_E_HI or more (linear in between); tuned by backtest
#endif
#ifndef SW_E_HI
#define SW_E_HI 30
#endif
#ifndef SWITCH_FIXED
#define SWITCH_FIXED 0 // test only: force one switch round
#endif
#ifndef DYN_DENSITY
#define DYN_DENSITY 6 // fleet cap before the switch: one dragon per this many cells (64-2 at most)
#endif
#ifndef QUEEN_SLOTS
#define QUEEN_SLOTS 3 // unit slots only the queen may fill (her survival split must never be refused)
#endif
#ifndef HALF_SPLIT
#define HALF_SPLIT 1 // 26: mid-game, a dragon this long splits in half (leaders too on congested maps)
#endif
#ifndef HALF_SPLIT_LEN
#define HALF_SPLIT_LEN 20
#endif
#ifndef PEARL_BEHIND_GAIN
#define PEARL_BEHIND_GAIN 4 // tail must be this much closer to use a pearl split
#endif
#ifndef PEARL_BEHIND_MAX
#define PEARL_BEHIND_MAX 10 // ...and the pearl at most this far from the tail
#endif
#ifndef SPLIT_MORE
#define SPLIT_MORE 1 // hunters keep splitting at EARLY_SPLIT_MIN_LEN for the first EARLY_SPLIT_ROUNDS (numbers first)
#endif
#ifndef LEADER_MIN_LEN
#define LEADER_MIN_LEN 6 // F1: a dragon elects itself leader only from this length (big maps had ~30 leaders, some L2)
#endif
#ifndef OPEN_PLAN
#define OPEN_PLAN 1 // until we have the majority: no hunters / wall / runners (all collect and split at 4); hunting
                    // starts with the majority. (The queen part - split to 2, explore alone - is OPEN_QUEEN, OFF.)
#endif
#ifndef OPEN_MAJ_UNITS
#define OPEN_MAJ_UNITS 8 // OPEN_PLAN majority needs at least this many dragons
#endif
#ifndef MAJ_ON
#define MAJ_ON 1.4 // majority from this many x their estimated dragons...
#endif
#ifndef MAJ_OFF
#define MAJ_OFF 1.1 // ...lost again below this
#endif
#ifndef QUEEN_START_SPLIT
#define QUEEN_START_SPLIT 0 // (OFF: over 408 games -7 wins, +14 queen deaths) user (2026-10-07): the queen splits at the beginning. Before QSS_END she splits in half
                            // (4 -> 2 + 2) whenever she can; only ENEMY heads within 3 stop it (the start crowd of our own
                            // dragons blocked every queen split before: she sat at length 4 to r150 on Australia). Her
                            // other split checks stay (calm, safe move, two real exits, room, child escape, loop, sealed start)
#endif
#ifndef QSS_END
#define QSS_END 30
#endif
#ifndef OPEN_QUEEN_SPLIT
#define OPEN_QUEEN_SPLIT 0 // (OFF: 194-10, Qd 40 - few extra splits, more queen deaths) O1 (user: multiply fast): during the opening plan (no majority yet) and before OQS_END the
                           // queen splits at length 4 like everyone else (2 + 2); all her split safety checks stay
#endif
#ifndef OQS_END
#define OQS_END 120
#endif
#ifndef OPEN_PUSH
#define OPEN_PUSH 0 // (neutral over 408 games: 194-10 / 191-13, Qd 35 / 36) O2 (user: occupy space): before OPEN_PUSH_END idle explorers are not kept out of their side and the
                    // border band (they arrive up to 3 sooner .. we arrive 4 sooner) pulls twice as hard
#endif
#ifndef OPEN_PUSH_END
#define OPEN_PUSH_END 100
#endif
#ifndef OPEN_HOLD
#define OPEN_HOLD 0 // (OFF: Around UNSW pearls r100 293 -> 199, 193-11) O3 (user: multiply fast; ladder #1 sits by spawn tiles: heads within 1 of a tile 60% vs our 38%):
                    // before OPEN_HOLD_END a spawn tile within HOLD_DIST steps is a pearl job if it spawns within
                    // HOLD_WAIT rounds (not only within distance + CAMP_LEAD) - waiting beats walking to a far pearl
#endif
#ifndef OPEN_HOLD_END
#define OPEN_HOLD_END 150
#endif
#ifndef HOLD_DIST
#define HOLD_DIST 3
#endif
#ifndef HOLD_WAIT
#define HOLD_WAIT 12
#endif
#ifndef OPEN_PLAN_END
#define OPEN_PLAN_END 300 // the opening plan ends here at the latest (or at the numbers -> length switch)
#endif

// ====================================================================================================
// SITUATION: PHASES, STRENGTH, MAP CLASS
// ====================================================================================================
#ifndef CONSOLIDATE
#define CONSOLIDATE 1 // (only with DYN_ALLOC 0; with it every map consolidates at the switch) big open maps: from CONSOL_ROUND no optional splits, spare dragons feed the queen (length wins there)
#endif
#ifndef CONSOL_ROUND
#define CONSOL_ROUND 350 // (only with DYN_ALLOC 0)
#endif
#ifndef OPEN_MIN_CELLS
#define OPEN_MIN_CELLS 1000 // smaller maps are congested: numbers matter more than length
#endif
#ifndef OPEN_KELP_MAX
#define OPEN_KELP_MAX 0.20 // share of known edges that are kelp above which a map counts as congested
#endif
#ifndef CONSOL_MIN_UNITS
#define CONSOL_MIN_UNITS 10 // (only with DYN_ALLOC 0; cons_floor() uses the enemy count) never feed the fleet below this (elimination)
#endif
#ifndef TIE_TRACK
#define TIE_TRACK 1 // 27: running guess of the round-500 tiebreaks steers feeding
#endif
#ifndef SAFE_TRADES
#define SAFE_TRADES 1 // no snipes / packs / pushes / even trades while outnumbered or in survival mode
#endif
#ifndef SURVIVAL
#define SURVIVAL 1 // survival mode: tiny fleet, heavily outnumbered or the last rounds
#endif
#ifndef SURV_UNITS
#define SURV_UNITS 3
#endif
#ifndef SURV_RATIO
#define SURV_RATIO 3 // survival when they have more than this many times our dragons...
#endif
#ifndef SURV_RATIO_MIN
#define SURV_RATIO_MIN 6 // ...and we know of at least this many of theirs
#endif
#ifndef LATE_SAFE
#define LATE_SAFE 1 // last rounds: the queen stops chasing pearls and keeps to safe ground
#endif
#ifndef LATE_SAFE_ROUNDS
#define LATE_SAFE_ROUNDS 40
#endif
#ifndef SMALL_MAP_CELLS
#define SMALL_MAP_CELLS 1200
#endif
#ifndef LOG_PHASE
#define LOG_PHASE 0 // test only: append the enemy pressure (P) to the status text
#endif
#ifndef ENEMY_EST
#define ENEMY_EST 1 // trade gates count enemies scaled by how much of the map our fleet sees (heard ids undercount a swarm)
#endif
#ifndef EST_VIEW_CELLS
#define EST_VIEW_CELLS 40 // cells one dragon effectively watches
#endif
#ifndef EST_MIN_COVER_X100
#define EST_MIN_COVER_X100 25 // coverage never assumed below this (percent)
#endif
#ifndef OPEN_FARM_ROUND
#define OPEN_FARM_ROUND 100 // before this round: no guards, screens, hunters or long queen chases - everyone farms
#endif

// ====================================================================================================
// ECONOMY: PEARLS, CAMPING, ZONES, TERRITORY
// ====================================================================================================
#ifndef UNKNOWN_EDGE_COST
#define UNKNOWN_EDGE_COST 1 // >1: route cost of an edge nobody has seen (benchmarked at 2: about 8 wins worse)
#endif
#ifndef TARGET_COMMIT
#define TARGET_COMMIT 1 // keep the current pearl unless another is clearly closer
#endif
#ifndef INTENT_MEMORY
#define INTENT_MEMORY 0 // 6c: zone / outpost / camp / explore / wall goals are kept unless a new one is clearly closer;
                        // (all OFF: every version loses ~3 Portals games) 1 = always (193-11, Qd 33, prot 81-3: slow to
                        // react), 2 = only while no enemy head is in view (195-9, Qd 35, prot 81-3);
                        // 3 = no override: the zone / outpost / camp choosers give last round's goal a bonus of
                        //     max(COMMIT_MARGIN, 30%) steps, so a goal they no longer allow (skipped, taken, crowded) drops out
                        //     (194-10, Qd 38, prot 81-3)
#endif
#ifndef COMMIT_MARGIN
#define COMMIT_MARGIN 4 // ... by this many steps and by 30%
#endif
#ifndef STALL_ROUNDS
#define STALL_ROUNDS 12 // no progress toward a pearl for this long: give it up (0 = off)
#endif
#ifndef STALL_SKIP
#define STALL_SKIP 30 // ... for this many rounds
#endif
#ifndef PREPOS_SLACK
#define PREPOS_SLACK 15 // head for a future pearl when its spawn is within travel time + this (not just 25 rounds)
#endif
#ifndef SKIP_PEARL_ROUNDS
#define SKIP_PEARL_ROUNDS 0 // >0: ignore a pearl this long once it is only reachable at a loss (benchmarked: hurts)
#endif
#ifndef CAMP_CONF
#define CAMP_CONF 0.85 // 2.3: confidence of an exactly known upcoming spawn (was 0.6)
#endif
#ifndef IDLE_CAMP
#define IDLE_CAMP 1 // 2.4: nothing to do and nothing unseen: camp the nearest free spawn tile
#endif
#ifndef OWN_HALF
#define OWN_HALF 1 // 17: pearls on our half count more; defenders (medium length) camp our half near the middle
#endif
#ifndef OWN_HALF_BONUS
#define OWN_HALF_BONUS 1.25
#endif
#ifndef DEFENDER_SLOT
#define DEFENDER_SLOT 5 // id-hash slot (0-9) of the defenders
#endif
#ifndef OPEN_CAMP_ROUNDS
#define OPEN_CAMP_ROUNDS 60 // opening: an idle dragon heads for a known spawn tile before exploring
#endif
#ifndef PAID_PEARL_RACE
#define PAID_PEARL_RACE 1 // a non-queen pays one segment to win a pearl race it would otherwise lose
#endif
#ifndef CAMP_JOB
#define CAMP_JOB 1 // 12: future spawns are jobs timed to arrive CAMP_LEAD rounds early; campers wait beside the tile
#endif
#ifndef CAMP_LEAD
#define CAMP_LEAD 3
#endif
#ifndef REGION_VALUE
#define REGION_VALUE 1 // pearls / camp spots in pearl-rich areas are worth more; the opening explores toward the centre
#endif
#ifndef RV_RAD
#define RV_RAD 4 // spawn tiles within this many cells make an area rich
#endif
#ifndef RV_PEARL_W
#define RV_PEARL_W 0.0 // (off: hurt Portals; 1.0 tested) a pearl in the richest area counts up to (1 + this) x a lone one
#endif
#ifndef RV_CAMP_W
#define RV_CAMP_W 0.0 // (off with RV_PEARL_W; 10 tested) camp spot: cells of travel the richest area is worth
#endif
#ifndef RV_EXPLORE_W
#define RV_EXPLORE_W 12.0 // opening exploration: cells of travel the map centre is worth
#endif
#ifndef RV_EXPLORE_END
#define RV_EXPLORE_END 120
#endif
#ifndef RV_FULL_CELLS
#define RV_FULL_CELLS 1600 // weights at full strength from this map area (40% at least on small maps)
#endif
#ifndef OUTPOSTS
#define OUTPOSTS 1 // idle collectors hold the pearl-richest spots (outposts) instead of drifting to the nearest tile
#endif
#ifndef OUTPOST_SEP
#define OUTPOST_SEP 8 // outposts at least this far apart
#endif
#ifndef OUTPOST_MIN_RV
#define OUTPOST_MIN_RV 0.35 // richness (0-1) an outpost needs
#endif
#ifndef OUTPOST_CROWD
#define OUTPOST_CROWD 6.0 // cells of travel each teammate already at an outpost costs
#endif
#ifndef OUTPOST_MIN_MARGIN
#define OUTPOST_MIN_MARGIN -2 // outposts only on our half or the middle (halfMargin >= this); -99 = anywhere
#endif
#ifndef CHASE_MAX
#define CHASE_MAX 20 // F4: rounds chasing one pearl (in total, target switches included) without eating: drop it
#endif
#ifndef CHASE_SKIP
#define CHASE_SKIP 40
#endif
#ifndef DRY_MAX
#define DRY_MAX 30 // F4: rounds on "pearl" without eating anything: drop the current target
#endif
#ifndef CENTRE_KNOWN
#define CENTRE_KNOWN 0 // (off: with the camp fix it cost Weakhold/Australia openings) F6: the opening centre pull stops once the centre is mostly seen and holds no spawn tiles
#endif
#ifndef OUTPOST_MIN_TILES
#define OUTPOST_MIN_TILES 0 // (off, tested 3: Weakhold opening 111 -> 70) F6: an outpost needs this many known spawn tiles within RV_RAD
#endif
#ifndef CAMP_MAX_WAIT
#define CAMP_MAX_WAIT 25 // F6: no camping on a tile whose next pearl is further off than this (explore instead)
#endif
#ifndef CAMP_GIVEUP
#define CAMP_GIVEUP 15 // F6: rounds by a camped tile with no pearl: leave it for 40 rounds
#endif
#ifndef VORONOI
#define VORONOI 1 // who reaches each cell first, by path: pearl contests, camping, idle exploring, the queen's pearls
#endif
#ifndef VOR_TURNS
#define VOR_TURNS 0 // 7h: territory by arrival TURN (cells / free steps per turn), not by cells
#endif
#ifndef VOR_REP_AGE
#define VOR_REP_AGE 3 // position reports this fresh (ours and theirs) seed the fields
#endif
#ifndef VOR_FRONT_BONUS
#define VOR_FRONT_BONUS 8.0 // idle exploring: cells of travel a cell on our side near the border is worth
#endif
#ifndef VOR_DEEP_PEN
#define VOR_DEEP_PEN 20.0 // idle exploring: cost of a cell deep in their territory (they arrive 4+ sooner)
#endif
#ifndef MATCH_SWAP
#define MATCH_SWAP 0 // 7i: after the greedy matching, swap pearls with a nearby teammate when the pair's total cost drops
#endif
#ifndef MATCH_SWAP_RANGE
#define MATCH_SWAP_RANGE 10
#endif
#ifndef MATCH_PATHS
#define MATCH_PATHS 0 // (tested, OFF: opening pearls fell - UNSW 555 -> 426/478, Islands 358 -> 332/292) teammates' cost to a pearl = path distance (bounded BFS), not manhattan
#endif
#ifndef MATCH_SELF_SAME
#define MATCH_SELF_SAME 1 // C: our own cost in the matching uses the same BFS as the teammates' (consistent metric)
#endif
#ifndef MATCH_DEPTH
#define MATCH_DEPTH 16 // BFS depth per teammate; beyond it the manhattan guess (at least MATCH_DEPTH + 1)
#endif
#ifndef ZONES
#define ZONES 1 // pearl-rich zones (own spawn knowledge + T_ZONE sonar reports); collectors spread over them in
                // proportion to richness: value = tiles / (teammates there + 1) - ZONE_DIST_W * travel
#endif
#ifndef DENSITY_BOX
#define DENSITY_BOX 0 // (OFF for v3.2.0: alone 195-9 Portals -3; in the combined build 194-10, prot 80-4) 7a: spawn-tile counts by 2D prefix sums over a box instead of the radius-r diamond loop
#endif
#ifndef ZONE_TILES
#define ZONE_TILES 4 // spawn tiles within RV_RAD that make a zone worth reporting
#endif
#ifndef ZONE_RAD
#define ZONE_RAD 5 // teammates this close to a zone are working it
#endif
#ifndef ZONE_DIST_W
#define ZONE_DIST_W 0.15 // tiles of richness one cell of travel costs
#endif
#ifndef ZONE_OVERRIDE
#define ZONE_OVERRIDE 8 // a pearl target further than this gives way to a better zone
#endif
#ifndef ZONE_MSG_EVERY
#define ZONE_MSG_EVERY 15
#endif
#ifndef LOOP_STALL
#define LOOP_STALL 1 // circling one 2x2 for LOOP_STALL_ROUNDS without eating while after a far pearl: drop that pearl
#endif
#ifndef LOOP_STALL_ROUNDS
#define LOOP_STALL_ROUNDS 8
#endif

// ====================================================================================================
// DEAD ENDS, DIVES, RECYCLING
// ====================================================================================================
#ifndef DIVE_MIN_GAIN
#define DIVE_MIN_GAIN 1 // net segments a dead-end dive must win (pearls eaten minus the 2 left behind)
#endif
#ifndef DIVE_DEPTH
#define DIVE_DEPTH 12 // moves the dive search looks ahead
#endif
#ifndef DIVE_NODE_CAP
#define DIVE_NODE_CAP 4000 // node budget per dive search
#endif
#ifndef DIVE_ENABLE
#define DIVE_ENABLE 1 // non-queens value dead ends by the dive search
#endif
#ifndef FRONTIER_OPEN
#define FRONTIER_OPEN 0 // 1: non-leaders treat a way into unexplored water as an exit (benchmarked: no gain)
#endif
#ifndef RECYCLE
#define RECYCLE 1 // 19: trapped with a teammate close by: self-destruct now so it eats the drop
#endif
#ifndef RECYCLE_MATE_DIST
#define RECYCLE_MATE_DIST 4
#endif
#ifndef DIVE_MATE_GAIN
#define DIVE_MATE_GAIN 0 // dead-end dive threshold when a teammate is near to eat the recycle drop
#endif
#ifndef POCKET_CHECK
#define POCKET_CHECK 1 // 21: through a portal or into a small enclosed area, the body must be able to keep moving
#endif
#ifndef DIVE_MAX_LEN
#define DIVE_MAX_LEN 12 // longest dragon that considers dead-end dives
#endif
#ifndef POCKET_NODE_CAP
#define POCKET_NODE_CAP 3000 // search budget of the pocket check (out of budget = assume fine)
#endif
#ifndef FOUNTAINS
#define FOUNTAINS 1 // tiles that refill within FOUNT_GAP rounds count as pearls in dives; no optional split inside a dead end
#endif
#ifndef SLOT_CHECK
#define SLOT_CHECK 1 // F7: stepping into a fully seen 1-wide dead end is a trap (non-queens) unless its pearls pay (dive)
#endif
#ifndef DEADEND_STRICT
#define DEADEND_STRICT 1 // P1: the no-split-in-a-dead-end rule only where every legal move leads into a 1-wide dead end
#endif
#ifndef FOUNT_GAP
#define FOUNT_GAP 3
#endif
#ifndef SURV_ECON
#define SURV_ECON 1 // survival mode: never step next to an enemy head for a pearl; no recycling in a small fleet
#endif
#ifndef SURV_THREAT_PEN
#define SURV_THREAT_PEN 200000.0
#endif
#ifndef RECYCLE_MIN_UNITS
#define RECYCLE_MIN_UNITS 6
#endif

#ifndef BODY_TRUST_VISION
#define BODY_TRUST_VISION 0 // B1 (tested, OFF): our body model never keeps a cell vision shows is not ours - 195-9,
                            // Portals 81-3: the queen forgot her real segments beyond a portal and hit herself
#endif

#ifndef LEGAL_BY_VISION
#define LEGAL_BY_VISION 1 // B1b: a visible cell with no dragon on it is always a legal step (the body model can hold
                          // stale segments after a portal crossing and made such moves look like hitting our tail)
#endif

#ifndef TOPO_TRAP
#define TOPO_TRAP 1 // 7b: a step across a bridge into a fully known branch with no loop longer than us is a trap
                    // (workers: unsafe unless a dive pays; queen: like a dead-end pocket). Any shape, any size.
#endif

// ====================================================================================================
// PORTALS
// ====================================================================================================
#ifndef PORTAL_SCOUTS
#define PORTAL_SCOUTS 1 // the hunter group (and portal role) opens unknown portals from round 0
#endif
#ifndef SCOUT_REACH_PCT
#define SCOUT_REACH_PCT 50 // scout portals only while we can reach less than this % of the map without them
#endif
#ifndef SCOUT_PEARL_MARGIN
#define SCOUT_PEARL_MARGIN 4 // a scout keeps its pearl only if it is this much closer than the portal
#endif
#ifndef PORTAL_ALWAYS
#define PORTAL_ALWAYS 1 // 2.6: scouts probe unknown portals whatever share of the map we can reach
#endif
#ifndef PORTAL_ROW
#define PORTAL_ROW 1 // 20: never step through a portal onto/next to a teammate's head (lower id has right of way)
#endif
#ifndef PORTAL_ROW_PEN
#define PORTAL_ROW_PEN 2000000
#endif
#ifndef BLIND_PORTAL_MAX_LEN
#define BLIND_PORTAL_MAX_LEN 3 // longest dragon that may cross a portal whose far side is unknown (can turn in a 2x2)
#endif
#ifndef PORTAL_RUSH
#define PORTAL_RUSH 0 // (off: v2.15.2 = v2.15.0 + fixes; 1 = v2.15.1) a short dragon that sees an unexplored portal close by goes through it at once (portals lead to
                      // regions nobody of ours has seen; one crossing teaches the whole fleet the pair)
#endif
#ifndef PORTAL_RUSH_PEARL
#define PORTAL_RUSH_PEARL 5 // a dragon with a pearl target this close collects it instead
#endif
#ifndef PORTAL_RUSH_DIST
#define PORTAL_RUSH_DIST 4 // water steps to the portal mouth
#endif

// ====================================================================================================
// QUEEN SAFETY: THREAT, TRAPS, LOOKAHEAD
// ====================================================================================================
#ifndef MAX_SPRINT
#define MAX_SPRINT 3 // free steps planned per turn (vision radius is 3)
#endif
#ifndef SURV_NODE_CAP
#define SURV_NODE_CAP 20000 // node budget of the queen's survival search
#endif
#ifndef SURV_UNKNOWN_SAFE
#define SURV_UNKNOWN_SAFE 0 // 1: queen survival search stops at an unexplored edge and calls it safe (old)
#endif
#ifndef QUEEN_THREAT_MAP
#define QUEEN_THREAT_MAP 1 // queen judges enemy reach by path distance (kelp, bodies), sonar reports included
#endif
#ifndef PAID_REACH
#define PAID_REACH 1 // enemy reach includes paid sprint steps (one segment each, down to length 2)
#endif
#ifndef PAID_REACH_CAP
#define PAID_REACH_CAP 24 // longest enemy sprint considered
#endif
#ifndef PAID_THREAT_PEN
#define PAID_THREAT_PEN 200000 // (QUEEN_HARD_THREAT 0) queen ending where an enemy needs k paid steps: PEN / k
#endif
#ifndef QUEEN_HARD_THREAT
#define QUEEN_HARD_THREAT 1 // any cell an enemy can reach with free + paid steps is near-forbidden for the queen
#endif
#ifndef HARD_THREAT_PEN
#define HARD_THREAT_PEN 1200000 // queen ending within an enemy's paid sprint reach (a known dead end costs 1e6)
#endif
#ifndef QUEEN_TRAP_PEN
#define QUEEN_TRAP_PEN 2500000 // queen move with no way out (certain death) must cost more than a mere threat
#endif
#ifndef THREAT_EXTRA_ATTACKER
#define THREAT_EXTRA_ATTACKER 300000 // per additional enemy that can reach the cell
#endif
#ifndef QUEEN_MARGIN
#define QUEEN_MARGIN 4 // steps beyond an enemy's reach the queen still prefers to widen
#endif
#ifndef QUEEN_MARGIN_PEN
#define QUEEN_MARGIN_PEN 20000 // per step of margin missing
#endif
#ifndef ENEMY_LEN_MEMORY
#define ENEMY_LEN_MEMORY 1 // enemy lengths: remember full sightings, guess when the body runs out of view
#endif
#ifndef UNSEEN_LEN_GUESS
#define UNSEEN_LEN_GUESS 4 // assumed length of an enemy whose body leaves our view (never seen whole; 6 boxed the queen in)
#endif
#ifndef QUEEN_LEN_MEM
#define QUEEN_LEN_MEM 100 // D: the queen's threat estimate trusts a remembered enemy length this long (others: LEN_MEM_ROUNDS)
#endif
#ifndef LEN_MEM_ROUNDS
#define LEN_MEM_ROUNDS 25 // a remembered length is trusted this long
#endif
#ifndef QUEEN_PAID_ESCAPE
#define QUEEN_PAID_ESCAPE 0 // every single step is inside some enemy's reach: sprint out, paying segments (off: with EVADE and
                            // STALKER_INTERCEPT it cost ~7 wins vs fleet11)
#endif
#ifndef QUEEN_EVADE
#define QUEEN_EVADE 0 // the queen drops pearls that lie in or next to an enemy's reach (off: see QUEEN_PAID_ESCAPE)
#endif
#ifndef QUEEN_SURV_RANK
#define QUEEN_SURV_RANK 1 // queen without a safe move prefers ways on over known dead ends
#endif
#ifndef QUEEN_AVOID_PORTAL
#define QUEEN_AVOID_PORTAL 1 // the queen never crosses portals and keeps off cells with portal edges
#endif
#ifndef QUEEN_AVOID_UNKNOWN
#define QUEEN_AVOID_UNKNOWN 1 // the queen keeps to explored water and does not explore
#endif
#ifndef QUEEN_LOOP_SAFE
#define QUEEN_LOOP_SAFE 1 // the queen only moves (and splits) where she can still reach a free loop longer than she is
#endif
#ifndef QUEEN_LOOP_DEPTH
#define QUEEN_LOOP_DEPTH 15 // steps searched for that 2x2 (through known open water only)
#endif
#ifndef QUEEN_LOOP_PEN
#define QUEEN_LOOP_PEN 1000000 // a move after which no such 2x2 is reachable (pockets and long corridors trap short queens)
#endif
#ifndef QUEEN_SEALED_NOSPLIT
#define QUEEN_SEALED_NOSPLIT 1 // queen starts in a kelp pocket closed within her first view (no portals): no optional splits
#endif
#ifndef QUEEN_LONG_LEN
#define QUEEN_LONG_LEN 12 // queens this long (feeding makes them) stay out of corridors
#endif
#ifndef QUEEN_CORRIDOR_PEN
#define QUEEN_CORRIDOR_PEN 300000
#endif
#ifndef QUEEN_INSURE
#define QUEEN_INSURE 1 // the queen about to be struck splits off all but 2: the length lives on in the child
#endif
#ifndef QUEEN_INSURE_LEN
#define QUEEN_INSURE_LEN 5
#endif
#ifndef QUEEN_CORRIDOR_MATE
#define QUEEN_CORRIDOR_MATE 1 // the queen does not enter a 1-wide corridor that leads to a teammate's head
#endif
#ifndef PURSUIT_SAFE
#define PURSUIT_SAFE 1 // queen: enemy heads move too - no corner cell with an enemy near, not every exit within their reach
#endif
#ifndef PURSUIT_DIST
#define PURSUIT_DIST 4
#endif
#ifndef CORNER_PEN
#define CORNER_PEN 1500000.0
#endif
#ifndef CORR_ENTER_PEN
#define CORR_ENTER_PEN 150000.0 // queen stepping into a 1-wide corridor with another dragon within 6 (a fifth of it otherwise)
#endif
#ifndef ALL_EXITS_PEN
#define ALL_EXITS_PEN 900000.0
#endif
#ifndef QUEEN_MOBILITY
#define QUEEN_MOBILITY 30000.0 // per free cell next to her destination while an enemy head is within 3 (open water over walls)
#endif
#ifndef THREAT_TAIL_FREE
#define THREAT_TAIL_FREE 1 // P3a: enemy reach passes through the queen's own tail cell (it moves away when she steps)
#endif
#ifndef QUEEN_NO_SUICIDE
#define QUEEN_NO_SUICIDE 0 // P3b (tested, OFF - no fewer queen deaths): no insurance split down to 2; boxed with 4+,
                           // she splits to stall rather than strike head-on
#endif
#ifndef QUEEN_LOOK2
#define QUEEN_LOOK2 1 // P3c: two-move lookahead - after this move, can she still reach next turn a cell no enemy can
                      // reach in two turns? If not, a big penalty; if so, the corner / all-exits rules stand down
#endif
#ifndef LOOK2_PEN
#define LOOK2_PEN 800000.0 // below HARD_THREAT_PEN: being inside a reach now is still worse
#endif
#ifndef LOOK2_CELL_BONUS
#define LOOK2_CELL_BONUS 15000.0 // per two-turn-safe cell she can reach next turn (up to 8)
#endif
#ifndef LOOK2_PAID
#define LOOK2_PAID 0 // her next-turn escape may use paid steps (0: free steps only, what she normally walks)
#endif
#ifndef LOOK2_SKIP_CORNER
#define LOOK2_SKIP_CORNER 1 // with a two-turn-safe way on, the corner / all-exits penalties are not applied
#endif
#ifndef LOOK2_QUEEN_REACH
#define LOOK2_QUEEN_REACH 6 // her own next-turn reach considered (free + paid steps)
#endif
#ifndef CONTACT_SEARCH
#define CONTACT_SEARCH 0 // R1 (contact.hpp): when an enemy head is close and the move is hard (R8 trigger), the queen
                         // searches two of her moves ahead with the nearest enemies' replies in between (max-min) and
                         // judges ground reached first + free loop + exits; blended into her score. 1 = on,
                         // 2 = compute and log only (QDIAG "C" lines), 0 = off
#endif
#ifndef CS_ALPHA
#define CS_ALPHA 0.3 // R1 blend weight (implicit-minimax style): score += CS_ALPHA * CS_SCALE * value (value -1 .. 1)
#endif
#ifndef CS_SCALE
#define CS_SCALE 2000000.0 // R1: size of the queen's safety terms (hard threat 1.2e6, corner 1.5e6)
#endif
#ifndef CS_ENEMIES
#define CS_ENEMIES 2 // R1: nearest enemy heads whose replies are searched (the rest: two-turn reach, as look-2)
#endif
#ifndef CS_REPLIES
#define CS_REPLIES 5 // R1: reply cells per enemy (closest to her head = most threatening)
#endif
#ifndef CS_NEAR
#define CS_NEAR 10 // R1: enemy heads this close (manhattan) can be searched enemies
#endif
#ifndef CS_TERR_RADIUS
#define CS_TERR_RADIUS 10 // R1: ground counted within this many of her steps
#endif
#ifndef CS_W_TERR
#define CS_W_TERR 0.55 // R1 leaf weights: ground she reaches first...
#endif
#ifndef CS_W_LOOP
#define CS_W_LOOP 0.30 // ...a free loop longer than her within reach...
#endif
#ifndef CS_W_EXITS
#define CS_W_EXITS 0.15 // ...her free exits (up to 3)
#endif
#ifndef CS_WORK_CAP
#define CS_WORK_CAP 400000 // R8: BFS cells one search may pop (~15M points); over it the search is dropped
#endif
#ifndef CS_TRIG_EXTRA
#define CS_TRIG_EXTRA 3 // R8 trigger: an enemy head within its full strike reach + this many of her head...
#endif
#ifndef CS_MARGIN
#define CS_MARGIN 200000.0 // R8 trigger: ...and (<= 1 fully safe move, or 2+ heads within 6, or best - second < this)
#endif
#ifndef ROLE_ORDER_FIX
#define ROLE_ORDER_FIX 1 // A1 (v3.3.0): hunter / guard / screen / defender are derived AFTER the leader election, heir
                         // and giant (they used last turn's leader flag and amQueen)
#endif
#ifndef INTENT_GUARDS
#define INTENT_GUARDS 1 // A3 / A4 / A7 (v3.3.0): later target steps no longer overwrite a guard's post (explore), a
                        // cork (pack, feed) or a pack regroup (portal, push, defend)
#endif
#ifndef REACH_EST
#define REACH_EST 1 // A6 (v3.3.0): leaders judge an enemy's free-step reach from its estimated length (body partly out of
                    // view), as the queen does - it used the visible part only
#endif
#ifndef QUEEN_CROWD
#define QUEEN_CROWD 2 // Q4: was 3 (fled too late, cornered: 196-8, Qd 38 -> 36, head-on 19 -> 17) - F5: this many enemy heads within CROWD_RAD of the queen: no pearls, no rest - get to open water (0 = off)
#endif
#ifndef CROWD_RAD
#define CROWD_RAD 6
#endif
#ifndef THREAT_RADIUS
#define THREAT_RADIUS 8 // an enemy head seen (or reported within 3 rounds) this close to her is a threat
#endif

#ifndef BAND_COMPARE
#define BAND_COMPARE 0 // 6a (tested, OFF): 2 = life band first, then the sum of the rest - changed 3 queen moves in 291k
                       // dragon-turns, suite 194-10 Qd 48 (noise); 1 = life > safety > goal + style - 194-10, Portals
                       // 81-3 (the queen gave up room for tiny safety gains); 0 = one plain sum (v2.16)
#endif

#ifndef TOPO_EVERY
#define TOPO_EVERY 1 // 7b/3c: topology rebuilt at most every this many rounds when new edges are known (CPU)
#endif
#ifndef QUEEN_TOPO_LEASH
#define QUEEN_TOPO_LEASH 0 // 7d: the queen may take pearls up to 2 x her leash from home where the pearl's loop region
                           // can hold her (topology) and we arrive first by 2+ (Voronoi)
#endif
#ifndef TOPO_LOOP
#define TOPO_LOOP 1 // 7b: queens too long for the k x k ring test (L >= 16) need a loop region longer than them nearby
#endif

// ====================================================================================================
// QUEEN: HOME, GROWTH, BUNKER
// ====================================================================================================
#ifndef QUEEN_SEED_UNITS
#define QUEEN_SEED_UNITS 3 // (only with EXPANSION 0) the queen may split off 2 only while the team is this small
#endif
#ifndef QUEEN_GROW_ROUND
#define QUEEN_GROW_ROUND 225 // (only with DYN_ALLOC 0; she grows from the switch) enemy queen still alive by now: our queen stops splitting and grows
#endif
#ifndef BUNKER
#define BUNKER 1 // enemy queen dead: ours trims to length 2 and circles a sheltered 2x2 with no pearls
#endif
#ifndef EQ_DEATH_DETECT
#define EQ_DEATH_DETECT 1 // infer the enemy queen's death from death drops and fleet reports, and keep gossiping it
#endif
#ifndef EQ_STRICT
#define EQ_STRICT 1 // death inference only from a death drop on her own last-seen body (no area or report guesses: 2 false alarms in 176 games)
#endif
#ifndef BUNKER_SEARCH
#define BUNKER_SEARCH 25 // path distance searched for a bunker box
#endif
#ifndef QUEEN_COMMIT
#define QUEEN_COMMIT 0 // the queen keeps her pearl too (target commitment), instead of switching every turn
#endif
#ifndef TIE_BUNKER
#define TIE_BUNKER 1 // late, both queens short and theirs no longer than ours: bunker at our length (no trim)
#endif
#ifndef TIE_BUNKER_ROUND
#define TIE_BUNKER_ROUND 350
#endif
#ifndef TIE_FORCE
#define TIE_FORCE 0 // test only: skip the enemy queen length check
#endif
#ifndef TIE_SEEN_ROUNDS
#define TIE_SEEN_ROUNDS 150 // how old the enemy queen's last exact length may be
#endif
#ifndef STARVE_RELAX
#define STARVE_RELAX 1 // the queen as (almost) the only eater, nothing eaten for a while: portal/unknown/loop rules relax
#endif
#ifndef STARVE_ROUNDS
#define STARVE_ROUNDS 20
#endif
#ifndef QUEEN_HOME
#define QUEEN_HOME 1 // the queen keeps to a leash round her spawn; pearls only inside it, away from enemy heads, never in pockets
#endif
#ifndef QUEEN_LEASH
#define QUEEN_LEASH 7
#endif
#ifndef QUEEN_SPLIT_UNITS
#define QUEEN_SPLIT_UNITS 8 // at home the queen splits only while the fleet is this small (her children crowd her)
#endif
#ifndef QUEEN_MIN_KEEP
#define QUEEN_MIN_KEEP 3 // (v2.14.4: was 4 - faster opening) a home queen never splits below this (their hidden queens are 2-3 long: the tiebreak stays ours)
#endif
#ifndef QUEEN_PEARL_CLEAR
#define QUEEN_PEARL_CLEAR 5 // no visible enemy head this close to a pearl she goes for
#endif
#ifndef LEAD_SAFE
#define LEAD_SAFE 1 // from LEAD_ROUND a queen longer than theirs (or >= LEAD_MIN_LEN, theirs unknown) makes no pearl trips
#endif
#ifndef LEAD_ROUND
#define LEAD_ROUND 300
#endif
#ifndef LEAD_MIN_LEN
#define LEAD_MIN_LEN 5
#endif
#ifndef EQ_INFER
#define EQ_INFER 1 // F2: their queen seen once and then not for EQ_GONE_ROUNDS = probably dead -> ours bunkers (no trim)
#endif
#ifndef EQ_GONE_ROUNDS
#define EQ_GONE_ROUNDS 100
#endif
#ifndef EQ_INFER_ROUND
#define EQ_INFER_ROUND 150
#endif
#ifndef QUEEN_FIRST_SPLIT
#define QUEEN_FIRST_SPLIT 1 // F3: with <= 3 dragons the queen may split at length 4 (keeps 2) - at the start her own
                            // dragons within 3 block it (`crowded`); unblocking it was tested as QUEEN_START_SPLIT: worse
#endif
#ifndef OPEN_QUEEN
#define OPEN_QUEEN 0 // (tested OFF: 191-13 vs 197-7 without it, same queen deaths) OPEN_PLAN queen part: splits down to 2 and explores when no teammate is near
#endif
#ifndef QUEEN_ISO_RAD
#define QUEEN_ISO_RAD 8 // no teammate this close: the queen is on her own and explores
#endif
#ifndef QUEEN_ADAPT
#define QUEEN_ADAPT 0 // P2 (tested, OFF): the home queen's leash follows the threat around her; in the opening she splits and
                      // eats freely. Lifts the opening but raises queen deaths (Islands, Trophy, protected maps) and leaves the
                      // queen at length 2 on pocket maps - revisit once P3 (queen two-move safety) is in.
#endif
#ifndef ADAPT_SPLIT
#define ADAPT_SPLIT 1 // (no effect: needs QUEEN_ADAPT, which is OFF) in the opening the queen splits at length 4
#endif
#ifndef ADAPT_FLEET
#define ADAPT_FLEET 6 // the opening lasts until the fleet first has this many dragons...
#endif
#ifndef ADAPT_OPEN_END
#define ADAPT_OPEN_END 150 // ...or until this round
#endif
#ifndef QUIET_ROUNDS
#define QUIET_ROUNDS 12 // rounds without a threat before her leash opens
#endif
#ifndef LEASH_MAX
#define LEASH_MAX 24 // after the opening the leash grows one cell per 2 quiet rounds up to this
#endif
#ifndef ADAPT_END_ROUND
#define ADAPT_END_ROUND 300 // after the opening the leash only grows before this round
#endif

// ====================================================================================================
// TEAMMATES AROUND THE QUEEN
// ====================================================================================================
#ifndef QUEEN_ROOM
#define QUEEN_ROOM 0 // 1: teammates near the queen measure her free space (they rarely see her whole body; no gain)
#endif
#ifndef QUEEN_SPACE_PEN
#define QUEEN_SPACE_PEN 40000 // teammate head ending within 2 of the queen's head (PEN / 3.5 at distance 3)
#endif
#ifndef QUEEN_CLEAR
#define QUEEN_CLEAR 0 // >0: teammates keep this path distance from the queen's head (benchmarked at 4: fewer wall deaths, more head-ons)
#endif
#ifndef QUEEN_CLEAR_PEN
#define QUEEN_CLEAR_PEN 30000 // per step closer than QUEEN_CLEAR; doubled in corridors
#endif
#ifndef QUEEN_ROOM_PEN
#define QUEEN_ROOM_PEN 150000
#endif
#ifndef TEAM_ADJ_PEN
#define TEAM_ADJ_PEN 4000 // non-leader ending next to a teammate's head (later movers see us anyway; dithers in corridors)
#endif
#ifndef QUEEN_EXIT_GUARD
#define QUEEN_EXIT_GUARD 1 // teammates never leave the queen fewer than 2 free exits (she moves first next round)
#endif
#ifndef QUEEN_EXIT_PEN
#define QUEEN_EXIT_PEN 2000000
#endif
#ifndef CORRIDOR_GUARD
#define CORRIDOR_GUARD 1 // teammates do not advance into a 1-wide corridor the queen is in
#endif

// ====================================================================================================
// OFFENCE: TRADES, HUNTERS, PACKS, WALL
// ====================================================================================================
#ifndef HUNT_QUEEN_RANGE
#define HUNT_QUEEN_RANGE 14 // non-queens within this path distance chase the enemy queen
#endif
#ifndef THREAT_PEN
#define THREAT_PEN 9000 // non-leader stepping next to an enemy head
#endif
#ifndef STALKER_INTERCEPT
#define STALKER_INTERCEPT 0 // any short teammate strikes an enemy head that threatens our queen (off: see QUEEN_PAID_ESCAPE)
#endif
#ifndef HUNTERS
#define HUNTERS 1 // part of the fleet hunts in coordinated packs (enemy queen first, then long dragons)
#endif
#ifndef HUNT_START
#define HUNT_START 60 // hunters start by this round (or as soon as expansion ends)
#endif
#ifndef HUNTER_SHARE
#define HUNTER_SHARE 3 // tenths of the non-leader fleet that hunt
#endif
#ifndef HUNTER_LEN
#define HUNTER_LEN 6 // hunters grow to this before going in (strike reach = free + L - 2 steps)
#endif
#ifndef HUNTER_SPLIT_LEN
#define HUNTER_SPLIT_LEN 12 // hunters still split from this length (both halves can strike); 0 = never
#endif
#ifndef PACK_SIZE
#define PACK_SIZE 3 // hunters sent at one long enemy dragon
#endif
#ifndef PREY_MIN_LEN
#define PREY_MIN_LEN 8 // enemy dragons this long are worth a small dragon in a head-on
#endif
#ifndef PACK_REGROUP
#define PACK_REGROUP 5 // a pack member this close to its prey waits for a mate (0 = off)
#endif
#ifndef ENDGAME_PACKS
#define ENDGAME_PACKS 1 // our queen dead: small dragons hunt in packs, long ones keep growing
#endif
#ifndef AGGRO_EXPANSION
#define AGGRO_EXPANSION 1 // while expanding: contest pearls nearer the enemy, push idle hunters toward its side
#endif
#ifndef PAID_REGICIDE
#define PAID_REGICIDE 1 // strike the visible enemy queen from as far as free + paid steps allow
#endif
#ifndef REGICIDE_MAX
#define REGICIDE_MAX 12
#endif
#ifndef SNIPE
#define SNIPE 1 // short dragons sprint-strike any visible enemy head at least as long as themselves
#endif
#ifndef SNIPE_MAX_LEN
#define SNIPE_MAX_LEN 6 // only dragons this short strike (cheap trades)
#endif
#ifndef HUNT_GATE
#define HUNT_GATE 1 // hunters only hunt while we are not outnumbered and not in survival mode
#endif
#ifndef MAJ_HUNT
#define MAJ_HUNT 1 // smaller maps: with a clear majority every spare dragon hunts enemy dragons (win by elimination)
#endif
#ifndef MAJ_RATIO_X10
#define MAJ_RATIO_X10 20 // majority = our dragons >= 2.0x the enemies we know of
#endif
#ifndef MAJ_MIN_UNITS
#define MAJ_MIN_UNITS 12
#endif
#ifndef LURK
#define LURK 1 // 30: pack members wait just outside her view, on different sides, and strike when she comes close
#endif
#ifndef LURK_DIST
#define LURK_DIST 4 // Chebyshev distance from her (her view reaches 3)
#endif
#ifndef LURK_FRESH
#define LURK_FRESH 8 // how old her reported position may be (sonar reports are rare)
#endif
#ifndef GIANT_HUNT
#define GIANT_HUNT 1 // 32: our queen dead, from GIANT_HUNT_ROUND, theirs long unseen: their longest dragons first
#endif
#ifndef GIANT_HUNT_ROUND
#define GIANT_HUNT_ROUND 350
#endif
#ifndef GIANT_PREY_MIN
#define GIANT_PREY_MIN 10
#endif
#ifndef CORK
#define CORK 1 // (locally identical: their queen is rarely seen; kept for the ladder) 7g: their queen in a corridor with one way out: take a corridor cell before she can leave
#endif
#ifndef CORK_RANGE
#define CORK_RANGE 12
#endif
#ifndef BOX_QUEEN
#define BOX_QUEEN 1 // 33: a dragon that sees their queen but cannot strike takes her free cells
#endif
#ifndef BOX_BONUS
#define BOX_BONUS 3000.0
#endif
#ifndef TRADE_UP
#define TRADE_UP 1 // head-on trades only for their queen, a dragon TRADE_UP_RATIO x our length, or a stalker of our queen
#endif
#ifndef TRADE_UP_RATIO
#define TRADE_UP_RATIO 2
#endif
#ifndef ANTI_ESCORT
#define ANTI_ESCORT 1 // an enemy head within 2 of our queen for ESCORT_ROUNDS is a stalker: reported, struck by short teammates
#endif
#ifndef ESCORT_ROUNDS
#define ESCORT_ROUNDS 2
#endif
#ifndef RESCUE_RANGE
#define RESCUE_RANGE 12 // teammates this far (path) from a stalker report come to strike it
#endif
#ifndef WALL_MIN_CELLS
#define WALL_MIN_CELLS 1500 // small / cramped maps: no wall (the line has nowhere to go and long members box our queen in)
#endif
#ifndef WALL_FORM_RATIO
#define WALL_FORM_RATIO 1.2 // the wall forms only while we have this many x their (estimated) dragons
#endif
#ifndef WALL_PUSH_RATIO
#define WALL_PUSH_RATIO 1.5 // the wall holds the middle line; it pushes on to their home only with this many x their dragons
#endif
#ifndef WALL
#define WALL 1 // from wall_round() collector slots WALL_SLOT_LO-HI join the hunter packs; with no prey they advance
               // side by side on the enemy's home (slot 9, guards and the defender stay back)
#endif
#ifndef WALL_SLOT_LO
#define WALL_SLOT_LO 6
#endif
#ifndef WALL_SLOT_HI
#define WALL_SLOT_HI 8
#endif
#ifndef WALL_MIN_LEN
#define WALL_MIN_LEN 3
#endif
#ifndef WALL_GAP
#define WALL_GAP 3 // cells between wall members across the line of advance
#endif
#ifndef WALL_LEAD
#define WALL_LEAD 4 // a member this much closer to the target than the others' average waits (collects nearby)
#endif
#ifndef HUNT_FAR_SMALL
#define HUNT_FAR_SMALL 1 // on big maps packs only go for their queen when she was seen in the last HUNT_FRESH rounds
#endif
#ifndef HUNT_FRESH
#define HUNT_FRESH 30
#endif

// ====================================================================================================
// DEFENCE: GUARDS, SCREENS, QUEEN NEWS
// ====================================================================================================
#ifndef GUARDS
#define GUARDS 1 // part of the fleet stays near our queen and intercepts enemies
#endif
#ifndef GUARD_SHARE
#define GUARD_SHARE 2 // tenths of the fleet that guard (hunters join once the enemy queen is dead)
#endif
#ifndef GUARD_RING
#define GUARD_RING 7 // guards come back when farther than this from the queen
#endif
#ifndef GUARD_MIN
#define GUARD_MIN 4 // guards never end a move this close to the queen's head (they screen, not crowd)
#endif
#ifndef GUARD_MIN_PEN
#define GUARD_MIN_PEN 60000 // per step inside GUARD_MIN
#endif
#ifndef CHOKE_GUARDS
#define CHOKE_GUARDS 1 // (user: keep; path search fixed to plan through unexplored water) 7j: idle guards hold the first bridge (topology) on the queen's way toward the enemy side
#endif
#ifndef CHOKE_RANGE
#define CHOKE_RANGE 10
#endif
#ifndef GUARD_INTERCEPT
#define GUARD_INTERCEPT 6 // enemy heads this close to the queen are intercepted
#endif
#ifndef GUARD_MIN_UNITS
#define GUARD_MIN_UNITS 0 // >0: no guard duty while the fleet is smaller than this or still expanding (0 = always)
#endif
#ifndef SCREENS
#define SCREENS 1 // the first guard slot screens: posted 6-9 cells from the queen toward the enemy, on her row or column
#endif
#ifndef SCREEN_MIN
#define SCREEN_MIN 6
#endif
#ifndef SCREEN_MAX
#define SCREEN_MAX 9
#endif
#ifndef AIM_REPORTS
#define AIM_REPORTS 1 // enemy news goes out in the queen's direction when she is on our row or column
#endif
#ifndef REP_MAX_AGE
#define REP_MAX_AGE 3 // teammates' enemy reports the queen's threat map still uses (rounds)
#endif
#ifndef QUEEN_POS_RELAY
#define QUEEN_POS_RELAY 1 // relay the queen's fresh status ahead of gossip (guards and screens need her position)
#endif
#ifndef GUARD_FLEET_MIN
#define GUARD_FLEET_MIN 5 // no guards or screens with this few dragons: everyone collects
#endif
#ifndef EQ_INFER_UNSEEN
#define EQ_INFER_UNSEEN 0 // Q2a: EQ_INFER also when their queen was never seen at all (she died unseen in 5 of 7 test games)
#endif
#ifndef GIANT_GOSSIP
#define GIANT_GOSSIP 0 // (OFF: still 8-14 giants - the sonar graph is in pieces) 6d'': one giant fleet-wide. STATUS is repacked (id 13, length 7, round mod 16 bits) to carry
                       // the best giant / heir claim its sender knows (length 5 bits, id hash 2, age 3); a holder that
                       // hears a better claim steps down. Spreads hop by hop like Q1; an age past GOSSIP_MAX_AGE ends a claim.
#endif
#ifndef GOSSIP_MAX_AGE
#define GOSSIP_MAX_AGE 28 // rounds a giant claim lives without its holder repeating it (one hop costs ~1 round: at 7 the
                          // top giant's own claim came back 9 rounds old and far giants never heard it)
#endif
#ifndef GOSSIP_AGE_UNIT
#define GOSSIP_AGE_UNIT 4 // the 3-bit age counts in units of this many rounds (rounded up: a claim never looks younger)
#endif
#ifndef SWITCH_STATUS_BIT
#define SWITCH_STATUS_BIT 0 // 6e: every STATUS carries the numbers -> length switch (today only T_PHASE does)
#endif
#ifndef STRIKE_NEWS
#define STRIKE_NEWS 0 // (OFF: 2.6% of its detections were right - lost relays look like silence) Q2c: a dragon within next-turn strike reach of their queen says so each turn (T_STRIKE), and
                      // "off" the turn after she got away; a striker dies in its strike and casts no sonar, so when the
                      // news stops with no "off" and nobody has seen her for STRIKE_CONFIRM rounds, she is dead.
                      // 1 = on, 2 = measure only (QDIAG log "N Q2C ...", no effect)
#endif
#ifndef STRIKE_CONFIRM
#define STRIKE_CONFIRM 6 // rounds of silence (no "off", no newer strike news, no sighting of her) before Q2c counts her dead
#endif
#ifndef EQ_STATUS_BIT
#define EQ_STATUS_BIT 1 // Q: every STATUS message carries "their queen is dead" (in 7 test games 47-90 of our dragons
                        // knew, our queen never did - she kept chasing pearls and died)
#endif
#ifndef BUNKER_GUARDS
#define BUNKER_GUARDS 1 // 6: the queen announces her bunker box; guards circle 2x2 posts on its open sides
#endif
#ifndef SCREENS_MORE
#define SCREENS_MORE 1 // 3: 2-3 screens in small fleets; screens face the nearest recent enemy; guards line up with the queen
#endif

// ====================================================================================================
// ENDGAME: FEEDING, GIANT, HEIR
// ====================================================================================================
#ifndef FEED_WINDOW
#define FEED_WINDOW 50 // last rounds in which farming may sacrifice feeders
#endif
#ifndef FEED_ONLY_IF_CONTEST
#define FEED_ONLY_IF_CONTEST 1 // no endgame feeding once the enemy queen is dead (ours just has to live)
#endif
#ifndef HEIR
#define HEIR 1 // enemy queen dead: our longest other dragon becomes a leader (no growth splits, pearl priority)
#endif
#ifndef LATE_LONGEST_ONLY
#define LATE_LONGEST_ONLY 30 // last rounds: without our queen, feed only the longest leader (total length is the last tiebreak)
#endif
#ifndef FEEDER_TRAIL
#define FEEDER_TRAIL 1 // 31: clusters of enemy death drops (their feeders) point at their queen
#endif
#ifndef GIANT
#define GIANT 1 // 25: open maps from CONSOL_ROUND: the longest non-queen leader is fed too (insurance)
#endif
#ifndef GIANT_SHARE
#define GIANT_SHARE 3 // one feeder in this many feeds the giant while the queen lives
#endif
#ifndef FEED_BEHIND_OPEN
#define FEED_BEHIND_OPEN 300 // behind on queen length: feeding starts here on open maps...
#endif
#ifndef FEED_BEHIND_CONGESTED
#define FEED_BEHIND_CONGESTED 400 // ...and here on congested ones
#endif
#ifndef FEED_CROWD
#define FEED_CROWD 3 // teammates already within 5 of the fed dragon: the rest wait
#endif
#ifndef CAP_FEED
#define CAP_FEED 1 // 24: fleet at its cap from CAP_FEED_ROUND and the queen safe: spare dragons feed her
#endif
#ifndef CAP_FEED_ROUND
#define CAP_FEED_ROUND 150
#endif
#ifndef QUEEN_FEED_CAP
#define QUEEN_FEED_CAP 16 // stop feeding her past max(this, their queen + QUEEN_FEED_MARGIN): a huge queen is hard to keep alive
#endif
#ifndef QUEEN_FEED_MARGIN
#define QUEEN_FEED_MARGIN 6
#endif
#ifndef GIANT_ALL
#define GIANT_ALL 1 // from GIANT_ROUND our longest non-queen dragon becomes a leader on every map (no growth splits,
                    // pearl priority, leader safety) and feeders feed it too: the longest-dragon tiebreak stays ours
#endif
#ifndef GIANT_ROUND
#define GIANT_ROUND 200
#endif
#ifndef GIANT_MIN_LEN
#define GIANT_MIN_LEN 5 // (10 tested: Portals 5 losses)
#endif
#ifndef ROLE_CLAIMS
#define ROLE_CLAIMS 0 // 6d (tested, OFF): giant / heir / leader holders announce their role (T_ROLE, relayed). Claims did
                      // not spread: on Around UNSW most giants heard 0 claims in 100 rounds (sonar stops at kelp and at the
                      // first dragon), still up to 16 giants and ~11 non-queen leaders at once
#endif
#ifndef ROLES_AFTER_SWITCH
#define ROLES_AFTER_SWITCH 0 // 6d' (tested, OFF: 196-8, Qd 40, Portals 81-3): giants and elected (non-queen) leaders only after the numbers -> length switch: before
                             // it they stop long dragons from splitting while the fleet should still grow (up to 16
                             // giants / 26 leaders at once on big maps; the switch is fleet-wide via T_PHASE)
#endif
#ifndef CLAIM_TTL
#define CLAIM_TTL 20 // a claim not renewed for this long is forgotten
#endif
#ifndef CLAIM_RELAY
#define CLAIM_RELAY 10 // a dragon passes one claimant's claim on at most once per this many rounds
#endif
#ifndef GIANT_ONE
#define GIANT_ONE 1 // F1: one giant/heir, only with GIANT_MIN_UNITS dragons; leaving the role drops the leader flag
#endif
#ifndef GIANT_MIN_UNITS
#define GIANT_MIN_UNITS 8
#endif
#ifndef FEED_FIX
#define FEED_FIX 1 // P4 (ladder trial; locally more queen deaths, 0 = v2.14.2): feeders die only where she can walk to the drop; drops near her are always hers; short queens fed earlier
#endif
#ifndef FEED_DELIVERY
#define FEED_DELIVERY 1 // (passed: 198-6, Qd 38, prot 84-0) 7e: a feeder dies only when 2/3 of its drop cells lie within FEED_DROP_DIST water steps of her head
#endif
#ifndef FEED_DROP_DIST
#define FEED_DROP_DIST 5
#endif
#ifndef FEED_PATH
#define FEED_PATH 3 // feeder head at most this many water steps from her head (straight-line distance put drops behind kelp)
#endif
#ifndef FEED_EAT_DIST
#define FEED_EAT_DIST 5 // pearls this close to her (path) that no enemy can reach are always hers (leash, late-safe aside)
#endif
#ifndef FEED_KEEP_RADIUS
#define FEED_KEEP_RADIUS 5 // from FEED_KEEP_ROUND teammates leave pearls this close to her head
#endif
#ifndef FEED_KEEP_ROUND
#define FEED_KEEP_ROUND 300
#endif
#ifndef FEED_UNKNOWN_GOAL
#define FEED_UNKNOWN_GOAL 8 // their queen unseen: ours counts as behind (earlier feeding) below this length
#endif
