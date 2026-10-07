// Fleet bot: numbers and information first, then size and control.
//
// Every dragon runs its own copy of this program and shares nothing but sonar,
// so the "fleet" behaviour is emergent: each dragon keeps a persistent model of
// the map, tells teammates what it learns, and makes decisions that stay
// consistent with what the others are likely to decide from the same facts.
//
// Queen update: dragons 0 and 1 are the two queens, and after round 500 the
// longer living queen wins. Our queen is the permanent leader: it never makes
// optional splits, takes no avoidable risk, gets first pick of pearls and is
// what the feeders feed. The enemy queen is worth any one of our other dragons.
// A dragon of length L moves ceil(L/4) steps a turn for free, so sprints are
// planned for pearls, escapes and queen kills.
//
// Per turn (strategy section 20):
//   1. read the 7x7 window into the world model (kelp, portals, pearls, timers)
//   2. read sonar: teammate status, pearls, portals, kelp, enemies, dangers
//   3. infer map symmetry and mirror everything learned onto the other half
//   4. estimate opponent density, pick phase / mode / leaders
//   5. choose one action: survival split, attack, feed, split, or best move
//   6. send four sonars (N, E, S, W) with the most useful facts

#include "helper.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace unswbc;

// ---------------------------------------------------------------------------
// Tunables (override with -D for local experiments)
// ---------------------------------------------------------------------------
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
#ifndef FEED_WINDOW
#define FEED_WINDOW 50 // last rounds in which farming may sacrifice feeders
#endif
#ifndef SPLIT_DENSITY
#define SPLIT_DENSITY 32 // at most one dragon per this many tiles (small maps get crowded)
#endif
#ifndef TRANS_SHIFT
#define TRANS_SHIFT 100 // leader phase starts this much earlier than the section-12 table (tuned)
#endif
#ifndef QUEEN_SEED_UNITS
#define QUEEN_SEED_UNITS 3 // the queen may split off 2 only while the team is this small
#endif
#ifndef HUNT_QUEEN_RANGE
#define HUNT_QUEEN_RANGE 14 // non-queens within this path distance chase the enemy queen
#endif
#ifndef MAX_SPRINT
#define MAX_SPRINT 3 // free steps planned per turn (vision radius is 3)
#endif
#ifndef SURV_NODE_CAP
#define SURV_NODE_CAP 20000 // node budget of the queen's survival search
#endif
#ifndef THREAT_PEN
#define THREAT_PEN 9000 // non-leader stepping next to an enemy head
#endif
#ifndef DIVE_MIN_GAIN
#define DIVE_MIN_GAIN 1 // net segments a dead-end dive must win (pearls eaten minus the 2 left behind)
#endif
#ifndef DIVE_DEPTH
#define DIVE_DEPTH 12 // moves the dive search looks ahead
#endif
#ifndef DIVE_NODE_CAP
#define DIVE_NODE_CAP 4000 // node budget per dive search
#endif
#ifndef EXPANSION
#define EXPANSION 1 // numbers first: everyone (queen included) splits from round 0 up to a denser cap
#endif
#ifndef EXP_DENSITY
#define EXP_DENSITY 12 // expansion cap: one dragon per this many tiles (64 at most)
#endif
#ifndef EXP_END
#define EXP_END 350 // after this round the normal SPLIT_DENSITY cap applies again
#endif
#ifndef QUEEN_GROW_ROUND
#define QUEEN_GROW_ROUND 225 // enemy queen still alive by now: our queen stops splitting and grows
#endif
#ifndef FEED_ONLY_IF_CONTEST
#define FEED_ONLY_IF_CONTEST 1 // no endgame feeding once the enemy queen is dead (ours just has to live)
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
#ifndef STALKER_INTERCEPT
#define STALKER_INTERCEPT 0 // any short teammate strikes an enemy head that threatens our queen (off: see QUEEN_PAID_ESCAPE)
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
#ifndef QUEEN_ROOM
#define QUEEN_ROOM 0 // 1: teammates near the queen measure her free space (they rarely see her whole body; no gain)
#endif
#ifndef QUEEN_SPACE_PEN
#define QUEEN_SPACE_PEN 40000 // teammate head ending within 2 of the queen's head (PEN / 3.5 at distance 3)
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
#ifndef GUARD_INTERCEPT
#define GUARD_INTERCEPT 6 // enemy heads this close to the queen are intercepted
#endif
#ifndef PORTAL_SCOUTS
#define PORTAL_SCOUTS 1 // the hunter group (and portal role) opens unknown portals from round 0
#endif
#ifndef SCOUT_REACH_PCT
#define SCOUT_REACH_PCT 50 // scout portals only while we can reach less than this % of the map without them
#endif
#ifndef SCOUT_PEARL_MARGIN
#define SCOUT_PEARL_MARGIN 4 // a scout keeps its pearl only if it is this much closer than the portal
#endif
#ifndef UNKNOWN_EDGE_COST
#define UNKNOWN_EDGE_COST 1 // >1: route cost of an edge nobody has seen (benchmarked at 2: about 8 wins worse)
#endif
#ifndef TARGET_COMMIT
#define TARGET_COMMIT 1 // keep the current pearl unless another is clearly closer
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
#ifndef QUEEN_CLEAR
#define QUEEN_CLEAR 0 // >0: teammates keep this path distance from the queen's head (benchmarked at 4: fewer wall deaths, more head-ons)
#endif
#ifndef QUEEN_CLEAR_PEN
#define QUEEN_CLEAR_PEN 30000 // per step closer than QUEEN_CLEAR; doubled in corridors
#endif
#ifndef QUEEN_ROOM_PEN
#define QUEEN_ROOM_PEN 150000
#endif
#ifndef DIVE_ENABLE
#define DIVE_ENABLE 1 // non-queens value dead ends by the dive search
#endif
#ifndef QUEEN_SURV_RANK
#define QUEEN_SURV_RANK 1 // queen without a safe move prefers ways on over known dead ends
#endif
#ifndef FRONTIER_OPEN
#define FRONTIER_OPEN 0 // 1: non-leaders treat a way into unexplored water as an exit (benchmarked: no gain)
#endif
#ifndef SKIP_PEARL_ROUNDS
#define SKIP_PEARL_ROUNDS 0 // >0: ignore a pearl this long once it is only reachable at a loss (benchmarked: hurts)
#endif
#ifndef GUARD_MIN_UNITS
#define GUARD_MIN_UNITS 0 // >0: no guard duty while the fleet is smaller than this or still expanding (0 = always)
#endif
#ifndef TEAM_ADJ_PEN
#define TEAM_ADJ_PEN 4000 // non-leader ending next to a teammate's head (later movers see us anyway; dithers in corridors)
#endif
#ifndef QUEEN_COMMIT
#define QUEEN_COMMIT 0 // the queen keeps her pearl too (target commitment), instead of switching every turn
#endif
#ifndef HEIR
#define HEIR 1 // enemy queen dead: our longest other dragon becomes a leader (no growth splits, pearl priority)
#endif
#ifndef QUEEN_AVOID_PORTAL
#define QUEEN_AVOID_PORTAL 1 // the queen never crosses portals and keeps off cells with portal edges
#endif
#ifndef QUEEN_AVOID_UNKNOWN
#define QUEEN_AVOID_UNKNOWN 1 // the queen keeps to explored water and does not explore
#endif
#ifndef SNIPE
#define SNIPE 1 // short dragons sprint-strike any visible enemy head at least as long as themselves
#endif
#ifndef SNIPE_MAX_LEN
#define SNIPE_MAX_LEN 6 // only dragons this short strike (cheap trades)
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
#ifndef QUEEN_EXIT_GUARD
#define QUEEN_EXIT_GUARD 1 // teammates never leave the queen fewer than 2 free exits (she moves first next round)
#endif
#ifndef QUEEN_EXIT_PEN
#define QUEEN_EXIT_PEN 2000000
#endif
#ifndef QUEEN_SEALED_NOSPLIT
#define QUEEN_SEALED_NOSPLIT 1 // queen starts in a kelp pocket closed within her first view (no portals): no optional splits
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
#ifndef CAMP_CONF
#define CAMP_CONF 0.85 // 2.3: confidence of an exactly known upcoming spawn (was 0.6)
#endif
#ifndef IDLE_CAMP
#define IDLE_CAMP 1 // 2.4: nothing to do and nothing unseen: camp the nearest free spawn tile
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
#ifndef PORTAL_ALWAYS
#define PORTAL_ALWAYS 1 // 2.6: scouts probe unknown portals whatever share of the map we can reach
#endif
#ifndef CONSOLIDATE
#define CONSOLIDATE 1 // big open maps: from CONSOL_ROUND no optional splits, spare dragons feed the queen (length wins there)
#endif
#ifndef CONSOL_ROUND
#define CONSOL_ROUND 350
#endif
#ifndef OPEN_MIN_CELLS
#define OPEN_MIN_CELLS 1000 // smaller maps are congested: numbers matter more than length
#endif
#ifndef OPEN_KELP_MAX
#define OPEN_KELP_MAX 0.20 // share of known edges that are kelp above which a map counts as congested
#endif
#ifndef CONSOL_MIN_UNITS
#define CONSOL_MIN_UNITS 10 // never feed the fleet below this (elimination)
#endif
#ifndef LATE_LONGEST_ONLY
#define LATE_LONGEST_ONLY 30 // last rounds: without our queen, feed only the longest leader (total length is the last tiebreak)
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
#ifndef RECYCLE
#define RECYCLE 1 // 19: trapped with a teammate close by: self-destruct now so it eats the drop
#endif
#ifndef RECYCLE_MATE_DIST
#define RECYCLE_MATE_DIST 4
#endif
#ifndef PORTAL_ROW
#define PORTAL_ROW 1 // 20: never step through a portal onto/next to a teammate's head (lower id has right of way)
#endif
#ifndef PORTAL_ROW_PEN
#define PORTAL_ROW_PEN 2000000
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
#ifndef TIE_TRACK
#define TIE_TRACK 1 // 27: running guess of the round-500 tiebreaks steers feeding
#endif
#ifndef TIE_MARGIN
#define TIE_MARGIN 5 // lead that counts as safe on queen / longest length
#endif
#ifndef FEED_BEHIND_OPEN
#define FEED_BEHIND_OPEN 300 // behind on queen length: feeding starts here on open maps...
#endif
#ifndef FEED_BEHIND_CONGESTED
#define FEED_BEHIND_CONGESTED 400 // ...and here on congested ones
#endif
#ifndef QUEEN_LONG_LEN
#define QUEEN_LONG_LEN 12 // queens this long (feeding makes them) stay out of corridors
#endif
#ifndef QUEEN_CORRIDOR_PEN
#define QUEEN_CORRIDOR_PEN 300000
#endif
#ifndef FEED_CROWD
#define FEED_CROWD 3 // teammates already within 5 of the fed dragon: the rest wait
#endif
// ---- v2.12: lessons from the Byte_Force / Jingling_munmin ladder games ----
#ifndef SAFE_TRADES
#define SAFE_TRADES 1 // no snipes / packs / pushes / even trades while outnumbered or in survival mode
#endif
#ifndef HUNT_GATE
#define HUNT_GATE 1 // hunters only hunt while we are not outnumbered and not in survival mode
#endif
#ifndef GUARD_FLEET_MIN
#define GUARD_FLEET_MIN 5 // no guards or screens with this few dragons: everyone collects
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
#ifndef STARVE_RELAX
#define STARVE_RELAX 1 // the queen as (almost) the only eater, nothing eaten for a while: portal/unknown/loop rules relax
#endif
#ifndef STARVE_ROUNDS
#define STARVE_ROUNDS 20
#endif
#ifndef CORRIDOR_GUARD
#define CORRIDOR_GUARD 1 // teammates do not advance into a 1-wide corridor the queen is in
#endif
#ifndef OPEN_CAMP_ROUNDS
#define OPEN_CAMP_ROUNDS 60 // opening: an idle dragon heads for a known spawn tile before exploring
#endif
#ifndef SPLIT_ROOM
#define SPLIT_ROOM 0 // (v2.14.4: off - faster splitting) growth splits only where the child has room (no children born into pockets)
#endif
#ifndef DIVE_MATE_GAIN
#define DIVE_MATE_GAIN 0 // dead-end dive threshold when a teammate is near to eat the recycle drop
#endif
#ifndef PAID_PEARL_RACE
#define PAID_PEARL_RACE 1 // a non-queen pays one segment to win a pearl race it would otherwise lose
#endif
// ---- v2.13 phase A ----
// ---- v2.13: dynamic allocation by enemy pressure (user design) ----
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
#ifndef MAJ_HUNT
#define MAJ_HUNT 1 // smaller maps: with a clear majority every spare dragon hunts enemy dragons (win by elimination)
#endif
#ifndef MAJ_RATIO_X10
#define MAJ_RATIO_X10 20 // majority = our dragons >= 2.0x the enemies we know of
#endif
#ifndef MAJ_MIN_UNITS
#define MAJ_MIN_UNITS 12
#endif
#ifndef SMALL_MAP_CELLS
#define SMALL_MAP_CELLS 1200
#endif
#ifndef QUEEN_INSURE
#define QUEEN_INSURE 1 // the queen about to be struck splits off all but 2: the length lives on in the child
#endif
#ifndef QUEEN_INSURE_LEN
#define QUEEN_INSURE_LEN 5
#endif
#ifndef LOG_PHASE
#define LOG_PHASE 0 // test only: append the enemy pressure (P) to the status text
#endif
#ifndef QUEEN_SLOTS
#define QUEEN_SLOTS 3 // unit slots only the queen may fill (her survival split must never be refused)
#endif
// ---- v2.13 phase C: offence (never while outnumbered or in survival mode, unless our queen is dead) ----
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
#ifndef BOX_QUEEN
#define BOX_QUEEN 1 // 33: a dragon that sees their queen but cannot strike takes her free cells
#endif
#ifndef BOX_BONUS
#define BOX_BONUS 3000.0
#endif
#ifndef BUNKER_GUARDS
#define BUNKER_GUARDS 1 // 6: the queen announces her bunker box; guards circle 2x2 posts on its open sides
#endif
#ifndef POCKET_CHECK
#define POCKET_CHECK 1 // 21: through a portal or into a small enclosed area, the body must be able to keep moving
#endif
#ifndef CAMP_JOB
#define CAMP_JOB 1 // 12: future spawns are jobs timed to arrive CAMP_LEAD rounds early; campers wait beside the tile
#endif
#ifndef CAMP_LEAD
#define CAMP_LEAD 3
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
#ifndef QUEEN_CORRIDOR_MATE
#define QUEEN_CORRIDOR_MATE 1 // the queen does not enter a 1-wide corridor that leads to a teammate's head
#endif
#ifndef HALF_SPLIT
#define HALF_SPLIT 1 // 26: mid-game, a dragon this long splits in half (leaders too on congested maps)
#endif
#ifndef HALF_SPLIT_LEN
#define HALF_SPLIT_LEN 20
#endif
#ifndef SCREENS_MORE
#define SCREENS_MORE 1 // 3: 2-3 screens in small fleets; screens face the nearest recent enemy; guards line up with the queen
#endif
#ifndef DIVE_MAX_LEN
#define DIVE_MAX_LEN 12 // longest dragon that considers dead-end dives
#endif
#ifndef POCKET_NODE_CAP
#define POCKET_NODE_CAP 3000 // search budget of the pocket check (out of budget = assume fine)
#endif
#ifndef BLIND_PORTAL_MAX_LEN
#define BLIND_PORTAL_MAX_LEN 3 // longest dragon that may cross a portal whose far side is unknown (can turn in a 2x2)
#endif
#ifndef PEARL_BEHIND_GAIN
#define PEARL_BEHIND_GAIN 4 // tail must be this much closer to use a pearl split
#endif
#ifndef PEARL_BEHIND_MAX
#define PEARL_BEHIND_MAX 10 // ...and the pearl at most this far from the tail
#endif
// ---- v2.14: lessons from the Stochastic Parrot ladder games ----
#ifndef ENEMY_EST
#define ENEMY_EST 1 // trade gates count enemies scaled by how much of the map our fleet sees (heard ids undercount a swarm)
#endif
#ifndef EST_VIEW_CELLS
#define EST_VIEW_CELLS 40 // cells one dragon effectively watches
#endif
#ifndef EST_MIN_COVER_X100
#define EST_MIN_COVER_X100 25 // coverage never assumed below this (percent)
#endif
#ifndef TRADE_UP
#define TRADE_UP 1 // head-on trades only for their queen, a dragon TRADE_UP_RATIO x our length, or a stalker of our queen
#endif
#ifndef TRADE_UP_RATIO
#define TRADE_UP_RATIO 2
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
#ifndef ANTI_ESCORT
#define ANTI_ESCORT 1 // an enemy head within 2 of our queen for ESCORT_ROUNDS is a stalker: reported, struck by short teammates
#endif
#ifndef ESCORT_ROUNDS
#define ESCORT_ROUNDS 2
#endif
#ifndef RESCUE_RANGE
#define RESCUE_RANGE 12 // teammates this far (path) from a stalker report come to strike it
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
#ifndef FOUNTAINS
#define FOUNTAINS 1 // tiles that refill within FOUNT_GAP rounds count as pearls in dives; no optional split inside a dead end
#endif
// ---- P3: queen survival (from 24 local head-on deaths) ----
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
// ---- P5: giant insurance on every map, and faster splitting ----
#ifndef GIANT_ALL
#define GIANT_ALL 1 // from GIANT_ROUND our longest non-queen dragon becomes a leader on every map (no growth splits,
                    // pearl priority, leader safety) and feeders feed it too: the longest-dragon tiebreak stays ours
#endif
#ifndef GIANT_ROUND
#define GIANT_ROUND 200
#endif
#ifndef GIANT_MIN_LEN
#define GIANT_MIN_LEN 5
#endif
#ifndef SPLIT_MORE
#define SPLIT_MORE 1 // hunters keep splitting at EARLY_SPLIT_MIN_LEN for the first EARLY_SPLIT_ROUNDS (numbers first)
#endif
// ---- P7: long-term pearl value (rich areas and the centre over lone local pearls) ----
#ifndef REGION_VALUE
#define REGION_VALUE 1 // pearls / camp spots in pearl-rich areas are worth more; the opening explores toward the centre
#endif
#ifndef RV_RAD
#define RV_RAD 4 // spawn tiles within this many cells make an area rich
#endif
#ifndef HARVEST
#define HARVEST 1 // v2.14.9 (user): from HARVEST_START, on every map, our dragons near the queen walk to her and die
                  // beside her head so she eats their bodies (round 500: the longer living queen wins). If she is
                  // dead, the same for our longest dragon (the next tiebreak). Only where we dominate: no enemy head
                  // within HARVEST_SAFE of the target or within 3 of the feeder. Everything before stays as it is.
#endif
#ifndef HARVEST_START
#define HARVEST_START 470
#endif
#ifndef HARVEST_ADAPT
#define HARVEST_ADAPT 1 // v5.0.1 (64 games: queen at r500 13.8 -> 18.2 avg, deaths 12 = 12; vs v5.0.0 17-15): behind on queen length (both queens alive, theirs known no shorter than ours) the harvest
                        // starts earlier: HARVEST_START - 10 - HARVEST_PER_GAP x gap, never before HARVEST_EARLIEST
#endif
#ifndef HARVEST_PER_GAP
#define HARVEST_PER_GAP 2
#endif
#ifndef HARVEST_UNKNOWN
#define HARVEST_UNKNOWN 450 // v5.0.1: their queen's length unknown (never seen lately) and ours under HARVEST_UNKNOWN_GOAL:
                            // start here (0 = wait for 470 as before)
#endif
#ifndef HARVEST_UNKNOWN_GOAL
#define HARVEST_UNKNOWN_GOAL 16
#endif
#ifndef HARVEST_EARLIEST
#define HARVEST_EARLIEST 400
#endif
#ifndef HARVEST_RADIUS
#define HARVEST_RADIUS 20 // dragons this many steps (path) from the target take part
#endif
#ifndef HARVEST_SAFE
#define HARVEST_SAFE 6 // no enemy head this close to the target (in view, or reported in the last 2 rounds)
#endif
#ifndef HARVEST_DIE_DIST
#define HARVEST_DIE_DIST 3 // die when the head is within this many cells (chebyshev) of the target's head (in view)
#endif
#ifndef DECOYS
#define DECOYS 1 // v2.14.8: a sonar ray whose first dragon (in view) is an enemy cannot reach a teammate - it carries
                 // one of THEIR own earlier messages back instead (stale news in their own valid format)
#endif
#ifndef DECOY_MIN_AGE
#define DECOY_MIN_AGE 10 // replay only enemy messages at least this many rounds old...
#endif
#ifndef DECOY_MAX_AGE
#define DECOY_MAX_AGE 60 // ...and at most this old (the stalest one in the window first)
#endif
#ifndef SIZE_CAP_PLAN
#define SIZE_CAP_PLAN 1 // user plan (v2.14.6). Before round X no dragon - the queen included - keeps length 4 or more
                        // (it splits in two), the fleet fills up to the unit limit and chases contested pearls.
                        // X: big open maps = the switch round clamped to CAP_X_MIN..CAP_X_MAX; small / congested
                        // maps = CAP_SMALL_END once we have been outnumbered (otherwise v2.14.5 as is). Open maps
                        // after X: no optional splits (dragons grow) and pearl-rich regions count more; from
                        // CAP_HARVEST every dragon (length 2 too) feeds the queen, or our longest dragon if she is dead
#endif
#ifndef CAP_X_MIN
#define CAP_X_MIN 300
#endif
#ifndef CAP_X_MAX
#define CAP_X_MAX 400
#endif
#ifndef CAP_SMALL_END
#define CAP_SMALL_END 400
#endif
#ifndef CAP_HARVEST
#define CAP_HARVEST 470
#endif
#ifndef CAP_RV_PEARL_W
#define CAP_RV_PEARL_W 1.0 // open maps after X: a pearl in the richest area counts up to (1 + this) x a lone one
#endif
#ifndef TOPO_TRAP
#define TOPO_TRAP 1 // v2.14.11 (from v3.x 7b): a step across a bridge into a fully known branch with no loop longer than
                    // us is a trap (others: unsafe, the dive logic decides; queen: like a dead-end pocket). Any shape.
#endif
#ifndef TOPO_LOOP
#define TOPO_LOOP 1 // v2.14.11 (from v3.x 7b): queens too long for the k x k ring test (L >= 16) need a loop region
                    // longer than them nearby (was: always fine)
#endif
#ifndef TOPO_EVERY
#define TOPO_EVERY 1 // topology rebuilt at most every this many rounds once new edges are known (CPU)
#endif
#ifndef CHOKE_GUARDS
#define CHOKE_GUARDS 1 // v2.14.12 safe version (v3.x 7j as is: 64 games, queen deaths 15 -> 20): only while a lower-id guard stays by her (CHOKE_RANGE 5). From v3.x 7j: idle guards (not screens) hold the first bridge (TOPO) on the queen's way
                       // toward the enemy side, within CHOKE_RANGE steps of her (not right next to her)
#endif
#ifndef CHOKE_RANGE
#define CHOKE_RANGE 5
#endif
#ifndef QUEEN_CAP_EXITS
#define QUEEN_CAP_EXITS 1 // v5.0.0: the size-cap queen split needs two real ways on, like her own split
#endif
#ifndef QUEEN_LOOK
#define QUEEN_LOOK 1 // v5.0.2: in contact (an enemy head in view within LOOK_NEAR) the queen searches LOOK_DEPTH of her own
                     // moves against every enemy's earliest arrival (free + paid steps, reports included). A line dies at
                     // turn k once some enemy can be on her head by then; lines are ranked by turns survived first, then
                     // by the ground she reaches strictly first and her fewest exits on the way (funnels)
#endif
#ifndef LOOK_NEAR
#define LOOK_NEAR 6
#endif
#ifndef LOOK_DEPTH
#define LOOK_DEPTH 4
#endif
#ifndef LOOK_DEATH_PEN
#define LOOK_DEATH_PEN 400000.0 // per turn this move's best line dies sooner than the best move's (below LOOK2_PEN)
#endif
#ifndef LOOK_TERR_W
#define LOOK_TERR_W 120000.0 // surviving lines: ground she reaches first (0..1 of 2L + 8 cells)
#endif
#ifndef LOOK_EXIT_W
#define LOOK_EXIT_W 60000.0 // surviving lines: fewest safe exits along the line (0..3, funnel detection)
#endif
#ifndef CAP_FIX
#define CAP_FIX 1 // v2.14.10: the size-cap plan and the harvest agree with the v2.14.5 rules (see the checks below)
#endif
#ifndef CAP_LATCH_BY
#define CAP_LATCH_BY 300 // CAP_FIX: a small map turns on the cap only if we are outnumbered before this round
#endif
#ifndef CAP_OPEN_LOCK
#define CAP_OPEN_LOCK 100 // CAP_FIX: the cap plan's open / congested verdict is fixed from this round
#endif
#ifndef CAP_RV_CAMP_W
#define CAP_RV_CAMP_W 10.0 // open maps after X: cells of travel the richest camp area is worth
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
// ---- P4: feeding that reaches the queen ----
#ifndef FEED_FIX
#define FEED_FIX 1 // P4 (ladder trial; locally more queen deaths, 0 = v2.14.2): feeders die only where she can walk to the drop; drops near her are always hers; short queens fed earlier
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
#ifndef QUEEN_ADAPT
#define QUEEN_ADAPT 0 // P2 (tested, OFF): the home queen's leash follows the threat around her; in the opening she splits and
                      // eats freely. Lifts the opening but raises queen deaths (Islands, Trophy, protected maps) and leaves the
                      // queen at length 2 on pocket maps - revisit once P3 (queen two-move safety) is in.
#endif
#ifndef ADAPT_SPLIT
#define ADAPT_SPLIT 1 // in the opening the queen splits at length 4 (no QUEEN_MIN_KEEP / QUEEN_SPLIT_UNITS)
#endif
#ifndef ADAPT_FLEET
#define ADAPT_FLEET 6 // the opening lasts until the fleet first has this many dragons...
#endif
#ifndef ADAPT_OPEN_END
#define ADAPT_OPEN_END 150 // ...or until this round
#endif
#ifndef THREAT_RADIUS
#define THREAT_RADIUS 8 // an enemy head seen (or reported within 3 rounds) this close to her is a threat
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
#ifndef OPEN_FARM_ROUND
#define OPEN_FARM_ROUND 100 // before this round: no guards, screens, hunters or long queen chases - everyone farms
#endif
#ifndef LOOP_STALL
#define LOOP_STALL 1 // circling one 2x2 for LOOP_STALL_ROUNDS without eating while after a far pearl: drop that pearl
#endif
#ifndef LOOP_STALL_ROUNDS
#define LOOP_STALL_ROUNDS 8
#endif
#ifndef HUNT_FAR_SMALL
#define HUNT_FAR_SMALL 1 // on big maps packs only go for their queen when she was seen in the last HUNT_FRESH rounds
#endif
#ifndef HUNT_FRESH
#define HUNT_FRESH 30
#endif

namespace {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
constexpr int INF = 1 << 29;
constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};
constexpr char DCH[4] = {'N', 'E', 'S', 'W'};

int W = 0, H = 0, N = 0;
int me = 0;
Team::Value myTeam = Team::A;
int R = 0;       // current round
int L = 3;       // my length
int units = 1;   // living dragons on my team
int unitLimit = 64;

int dir_index(Direction d)
{
    switch (d.value)
    {
    case Direction::NORTH: return 0;
    case Direction::EAST: return 1;
    case Direction::SOUTH: return 2;
    default: return 3;
    }
}
Direction dir_of(int i) { return Direction(DCH[i]); }

int cell_of(int x, int y) { return y * W + x; }
int cell_of(Position p) { return cell_of(p.x, p.y); }

int nb(int c, int d)
{
    int x = c % W + DX[d], y = c / W + DY[d];
    if (x < 0) x += W;
    else if (x >= W) x -= W;
    if (y < 0) y += H;
    else if (y >= H) y -= H;
    return y * W + x;
}

int wrap_dist(int a, int b, int n)
{
    int const d = std::abs(a - b);
    return std::min(d, n - d);
}
int manhattan(int a, int b) { return wrap_dist(a % W, b % W, W) + wrap_dist(a / W, b / W, H); }
int cheb(int a, int b) { return std::max(wrap_dist(a % W, b % W, W), wrap_dist(a / W, b / W, H)); }

std::mt19937 rng;

// ---------------------------------------------------------------------------
// World model: edges and portals
// ---------------------------------------------------------------------------
// Every edge is stored once. code = cell*2 + o, where o = 0 is the horizontal
// edge on the NORTH side of the cell and o = 1 the vertical edge on its WEST
// side. The south/east edges of a cell are the north/west edges of its
// southern/eastern neighbour (the map wraps).
enum : std::uint8_t { E_UNK = 0, E_OPEN = 1, E_KELP = 2, E_PORTAL = 3 };

std::vector<std::uint8_t> edgeType; // best knowledge (observed, told or mirrored)
std::vector<std::uint8_t> obsEdge;  // observed by us or a teammate (symmetry test)
std::vector<int> edgePid;           // portal id, -1 unknown
std::vector<int> edgePartner;       // partner edge code, -1 unknown
int edgeVersion = 0;                // bumped whenever edgeType / edgePartner change (topology rebuilds)
std::unordered_map<int, std::vector<int>> portalById;
std::vector<int> kelpList;                      // known kelp codes (gossip)
std::vector<std::pair<int, int>> portalPairs;   // known pairs (gossip)

int edge_code(int c, int d)
{
    switch (d)
    {
    case 0: return c * 2;
    case 1: return nb(c, 1) * 2 + 1;
    case 2: return nb(c, 2) * 2;
    default: return c * 2 + 1;
    }
}

// Where a head standing on c ends up after stepping in direction d.
//   >= 0 : the destination cell
//     -1 : kelp (death)
//     -2 : edge never seen
//     -3 : portal whose partner we have not located
// A portal keeps the heading: the head leaves the partner edge on its d side
// (verified in the engine with a test map).
int cross(int c, int d)
{
    int const code = edge_code(c, d);
    switch (edgeType[code])
    {
    case E_OPEN: return nb(c, d);
    case E_KELP: return -1;
    case E_UNK: return -2;
    default: break;
    }
    int const p = edgePartner[code];
    if (p < 0)
        return -3;
    int const pc = p >> 1;
    if ((p & 1) == 0) // partner is the north edge of pc
        return d == 2 ? pc : nb(pc, 0);
    return d == 1 ? pc : nb(pc, 3); // partner is the west edge of pc
}

// ---------------------------------------------------------------------------
// Symmetry (section 10)
// ---------------------------------------------------------------------------
// 0: mirror x -> W-1-x   1: mirror y -> H-1-y   2: rotate 180 degrees.
// These formulas were checked against every bundled map file.
int symMask = 7;
int symKnown = -1;
int symConfirm[3] = {0, 0, 0};
bool symFailed = false;

int mirror_cell(int k, int c)
{
    int x = c % W, y = c / W;
    if (k != 1) x = W - 1 - x;
    if (k != 0) y = H - 1 - y;
    return cell_of(x, y);
}

int mirror_edge(int k, int code)
{
    int const m = mirror_cell(k, code >> 1);
    if ((code & 1) == 0)
        return (k != 0 ? nb(m, 2) : m) * 2; // north edge maps to the mirrored south edge when y flips
    return (k != 1 ? nb(m, 1) : m) * 2 + 1; // west edge maps to the mirrored east edge when x flips
}

// ---------------------------------------------------------------------------
// World model: tiles and pearls (sections 6, 18)
// ---------------------------------------------------------------------------
std::vector<int> seenRound;  // last round the tile was in view, -1 never
std::vector<int> obsNext;    // observed next spawn round, -1 unknown, -2 never spawns
std::vector<int> nextSpawn;  // best estimate incl. mirrored/predicted, same encoding
std::vector<char> nextEst;   // nextSpawn is an estimate from observed gaps
std::vector<int> pearlState; // 0 none, 1 present (seen), 2 predicted
std::vector<int> dropRound;  // round a death-drop pearl (non-spawn tile) was first seen
std::vector<int> pearlRound; // round the pearlState info refers to
struct DropEv
{
    int cell, round;
    bool nearOurQueen; // likely one of our own feeders
};
std::vector<DropEv> dropEv; // death drops seen or heard of (non-spawn tiles), newest last
std::vector<int> gapLo, gapHi; // observed respawn gaps (0 = unknown)

// ---------------------------------------------------------------------------
// This turn's view of dragons
// ---------------------------------------------------------------------------
std::vector<char> vis;       // in view this turn
std::vector<int> occId;      // dragon id on the tile (in view), -1 none
std::vector<char> occMine;   // that dragon is on my team
std::vector<char> occHead;
std::vector<int> occDir;
std::vector<int> otherRel; // 0 free, else the earliest step at which that segment is gone
std::vector<char> contested; // free now, but another head could step here before our next move

struct HeadInfo
{
    int cell, id, len, dir;
    bool mine;
    int estLen = 0;       // enemies: best length estimate for strike reach (len counts only what is in view)
    bool partial = false; // body runs out of view
};
std::vector<HeadInfo> heads; // other dragons' heads in view
std::vector<int> enemyTails; // visible enemy tails that could split: a child's head appears there this round

std::vector<int> body; // my body, head first (may be shorter than L if unseen)
std::vector<int> prevBody;
std::vector<int> lastDirs; // steps of last turn's move (empty after a split)
std::unordered_map<int, int> prevTeamHeads; // teammate id -> head cell last turn

// ---------------------------------------------------------------------------
// Fleet knowledge (sonar)
// ---------------------------------------------------------------------------
struct Status
{
    int len, cell, round;
    bool leader;
};
std::unordered_map<int, Status> fleet;

struct EnemyRep
{
    int cell, round, id, len;
    int estLen = 0; // queen threat only: length guess when the body was not seen whole
};
std::vector<EnemyRep> enemyReps;
struct LenMem
{
    int len, round;
    bool exact;
};
std::unordered_map<int, LenMem> enemyLenMem; // enemy id -> last observed length (exact = whole body seen)

// Length of an enemy for its strike reach: exact when the whole body is in
// view, otherwise the last exact sighting, and at least a guess when we never
// saw it whole (a long hunter shows only its head at the edge of our window).
int est_enemy_len(int id, int seen, bool partial)
{
    if (!ENEMY_LEN_MEMORY || !partial)
        return seen;
    auto const it = enemyLenMem.find(id);
    if (it != enemyLenMem.end() && R - it->second.round <= LEN_MEM_ROUNDS)
        return std::max({seen, it->second.len, it->second.exact ? 0 : UNSEEN_LEN_GUESS});
    return std::max(seen, UNSEEN_LEN_GUESS);
}
void remember_enemy_len(int id, int len, bool exact)
{
    auto it = enemyLenMem.find(id);
    if (exact || it == enemyLenMem.end() || R - it->second.round > LEN_MEM_ROUNDS)
        enemyLenMem[id] = {len, R, exact};
    else if (!it->second.exact || len > it->second.len)
        it->second = {std::max(it->second.len, len), R, it->second.exact && len <= it->second.len};
}

struct Danger
{
    int cell, round, cause; // cause 1 = enemy involved, 2 = hazard / trap
};
std::vector<Danger> dangers;

struct Claim
{
    int id, eta, round;
};
std::unordered_map<int, Claim> claims; // pearl cell -> claim

// ---------------------------------------------------------------------------
// Strategy state
// ---------------------------------------------------------------------------
enum Role { ROLE_COLLECTOR, ROLE_EXPLORER, ROLE_PORTAL, ROLE_COMBAT };
enum Mode { MODE_NORMAL, MODE_FARM, MODE_PRESERVE, MODE_COMBAT };

Role role = ROLE_COLLECTOR;
Mode mode = MODE_NORMAL;
bool isLeader = false;
bool survivalMode = false, outnumbered = false, lateSafe = false; // v2.12
int lastEatR = 0, lastLenSeen = 0;
bool isDefender = false; // holds our half (medium length, camps near the middle)
int qBunkerTl = -1, qBunkerR = -1, inferTl = -1, inferR = -1; // the queen's bunker box as she last announced it
int pressureMax = 0, switchRound = SW_HI, firstR = -1; // DYN_ALLOC
bool switched = false, heardPressure = false, majorityHunt = false;
bool capMode = false, capSmallLatched = false; int capRound = 0; // SIZE_CAP_PLAN
bool capOpen = false; int capOpenLock = -1; // CAP_FIX: the open verdict the cap plan uses (fixed from CAP_OPEN_LOCK)
bool openMap = false;     // big map with little kelp: long dragons do not trap themselves
bool consolidate = false; // big open map, late: length over numbers
bool isScreen = false; // guard posted out toward the enemy to see attackers early
bool isHeir = false; // enemy queen dead: the dragon that grows instead of our queen
bool isGiant = false; // P5: our longest non-queen dragon from GIANT_ROUND (insurance for the longest-dragon tiebreak)
bool sealedChecked = false, queenSealed = false;
int sealedCells = 0; // size of the queen's sealed starting pocket
int lastEqSeenR = -1000; // last round their queen was seen or reported
int enemyEst = 0;          // v2.14: enemy count for the trade gates (scaled by our coverage)
bool queenStarvingG = false; // v2.14: the queen's starving flag, for pearl gathering
std::unordered_map<int, std::pair<int, int>> escortSeen; // enemy id -> (last round within 2 of our queen, rounds in a row)
std::vector<int> headHist;  // our head, last rounds (loop stall)
int restCell = -1;          // v2.14: the queen's resting spot (open water inside her leash)
int queenLeash = QUEEN_LEASH; // P2: her leash this turn
int lastThreatR = -1000;      // last round an enemy head was near her
bool fleetEstablished = false; // the fleet once had ADAPT_FLEET dragons: the opening is over
bool queenOpening = false;     // the queen splits and roams as before the home rule
bool miningTrip = false;    // v2.15: on a dead-end trip that paid >= 1 length when we went in
int queenId = 0, enemyQueenId = 1; // ids 0 and 1 are the queens; which is ours is learned
bool amQueen = false;
bool ourQueenDead = false, enemyQueenDead = false;
int prevEnemyQueenCell = -1;
int prevEnemyQueenLen = 0;
std::vector<int> prevEnemyQueenBody, eqBodyNow; // her body cells we saw last turn / this turn
int ourQueenSpawn = -1; // where our queen started (the enemy queen started at its mirror image)
int lastPearl = -1;     // the pearl this dragon went for last turn
int stallTarget = -1, stallBest = INF, stallRound = 0; // progress toward the current pearl
bool isHunter = false;
bool isGuard = false;
bool hunterSlot = false; // in the hunter group (scouts portals before the hunt starts)
bool queenSeen = false; // queen identity confirmed by vision (team is shown there)
bool transitioned = false;
bool expansionPhase = true; // numbers first, until the fleet first reaches its expansion target
bool queenSplits = true;    // the queen splits rather than grows (see QUEEN_GROW_ROUND)
int transitionRound = 300;
int localThreat = 0;
int distinctEnemies = 0;
int echoEnemy = 0;
int exploreGoal = -1, exploreSetRound = -1000;
int claimTarget = -1, claimEta = 0;
std::string note;

// ---------------------------------------------------------------------------
// Sonar messages
// ---------------------------------------------------------------------------
// v2.14.7 (encrypted sonar, from v3.x step 7f): 64 bits = [63..12] the 52-bit block (type 4 bits << 48 | payload
// 48 bits) under a keyed 4-round Feistel cipher, [11..0] a 12-bit check code of the plain block. Replays are public
// and show sonar values: without the key nobody can read our messages (the queen's position is in her STATUS) or
// forge them; salted by team. A message is accepted only if the code matches, the type is one we send and every
// payload bit past that type's layout is zero, so a stray value almost never passes.
enum MsgType
{
    T_STATUS = 1,
    T_PEARL = 2,
    T_PORTAL = 3,
    T_KELP = 4,
    T_ENEMY = 5,
    T_DANGER = 6,
    T_SYM = 7,
    T_CLAIM = 8,
    T_PORTAL_EDGE = 9,
    T_QUEEN_DEAD = 10,
    T_QSPAWN = 11,
    T_PHASE = 12,
    T_BUNKER = 13,
};

struct BitW
{
    std::uint64_t v = 0;
    int pos = 0;
    void put(std::uint64_t x, int bits)
    {
        v |= (x & ((1ULL << bits) - 1)) << pos;
        pos += bits;
    }
};
struct BitR
{
    std::uint64_t v;
    int pos = 0;
    explicit BitR(std::uint64_t v) : v(v) {}
    int get(int bits)
    {
        int const x = static_cast<int>((v >> pos) & ((1ULL << bits) - 1));
        pos += bits;
        return x;
    }
};

#ifndef MSG_KEY
#define MSG_KEY 0x6A09E667F3BCC908ULL // secret: change it to change every message
#endif
std::uint64_t mix64(std::uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}
std::uint64_t team_key() { return MSG_KEY ^ (myTeam == Team::A ? 0x3C6EF372FE94F82BULL : 0xA54FF53A5F1D36F1ULL); }
constexpr int HALF_BITS = 26;
constexpr std::uint64_t HALF_MASK = (1ULL << HALF_BITS) - 1;
std::uint64_t feistel_f(std::uint64_t half, int round) { return mix64(half ^ team_key() ^ (0x9E3779B97F4A7C15ULL * (round + 1))) & HALF_MASK; }
std::uint64_t encrypt52(std::uint64_t block)
{
    std::uint64_t l = block >> HALF_BITS, r = block & HALF_MASK;
    for (int k = 0; k < 4; k++)
    {
        std::uint64_t const t = r;
        r = l ^ feistel_f(r, k);
        l = t;
    }
    return (l << HALF_BITS) | r;
}
std::uint64_t decrypt52(std::uint64_t block)
{
    std::uint64_t l = block >> HALF_BITS, r = block & HALF_MASK;
    for (int k = 3; k >= 0; k--)
    {
        std::uint64_t const t = l;
        l = r ^ feistel_f(l, k);
        r = t;
    }
    return (l << HALF_BITS) | r;
}
std::uint64_t check_code(std::uint64_t block) { return mix64(block ^ ~team_key()) >> 52; }
// Payload bits each type uses (the rest must be zero). PEARL and KELP carry a count in their lowest bits.
int layout_bits(int type, std::uint64_t payload)
{
    switch (type)
    {
    case T_STATUS: return 46;
    case T_PEARL: return 1 + (static_cast<int>(payload & 1) + 1) * 23;
    case T_PORTAL: return 26;
    case T_KELP: return 2 + static_cast<int>(payload & 3) * 13;
    case T_ENEMY: return 44;
    case T_DANGER: return 24;
    case T_SYM: return 2;
    case T_CLAIM: return 43;
    case T_PORTAL_EDGE: return 33;
    case T_QUEEN_DEAD: return 3;
    case T_QSPAWN: return 12;
    case T_PHASE: return 16;
    case T_BUNKER: return 21;
    default: return -1; // not a type we send
    }
}

std::uint64_t pack(int type, std::uint64_t payload)
{
    payload &= (1ULL << 48) - 1;
    std::uint64_t const block = (static_cast<std::uint64_t>(type) << 48) | payload;
    return (encrypt52(block) << 12) | check_code(block);
}

bool unpack(std::uint64_t m, int& type, std::uint64_t& payload)
{
    std::uint64_t const block = decrypt52(m >> 12);
    if ((m & 0xFFF) != check_code(block))
        return false;
    type = static_cast<int>(block >> 48);
    payload = block & ((1ULL << 48) - 1);
    int const bits = layout_bits(type, payload);
    return bits >= 0 && (payload >> bits) == 0;
}

struct Fact
{
    std::uint64_t msg;
    int prio, sends;
};
// DECOYS: messages we received that are not ours (they fail our check code): the enemy's own.
struct Foreign
{
    std::uint64_t msg;
    int round;
};
std::vector<Foreign> foreignMsgs;
int decoysSent = 0;
std::vector<Fact> facts;

void queue_fact(std::uint64_t msg, int prio, int sends)
{
    for (auto& f : facts)
        if (f.msg == msg)
        {
            f.prio = std::max(f.prio, prio);
            f.sends = std::max(f.sends, sends);
            return;
        }
    facts.push_back({msg, prio, sends});
    if (facts.size() > 64)
    {
        auto it = std::min_element(facts.begin(), facts.end(), [](Fact const& a, Fact const& b) { return a.prio < b.prio; });
        facts.erase(it);
    }
}

struct PearlEntry
{
    int kind, cell, round, prio; // kind 0 next spawn at round, 1 present at round, 2 gone at round
};
std::vector<PearlEntry> pearlQ;

void queue_pearl(int kind, int cell, int round, int prio)
{
    for (auto& e : pearlQ)
        if (e.cell == cell && e.kind == kind)
        {
            e.round = round;
            e.prio = std::max(e.prio, prio);
            return;
        }
    pearlQ.push_back({kind, cell, round, prio});
    if (pearlQ.size() > 40)
    {
        auto it = std::min_element(pearlQ.begin(), pearlQ.end(), [](PearlEntry const& a, PearlEntry const& b) { return a.prio < b.prio; });
        pearlQ.erase(it);
    }
}

std::uint64_t msg_status()
{
    BitW b;
    b.put(me, 14);
    b.put(std::min(L, 1023), 10);
    b.put(body.empty() ? 0 : body[0], 12);
    b.put(isLeader ? 1 : 0, 1);
    b.put(R, 9);
    return pack(T_STATUS, b.v);
}
std::uint64_t msg_portal(int a, int c)
{
    BitW b;
    b.put(a, 13);
    b.put(c, 13);
    return pack(T_PORTAL, b.v);
}
std::uint64_t msg_portal_edge(int code, int pid)
{
    BitW b;
    b.put(code, 13);
    b.put(pid, 20);
    return pack(T_PORTAL_EDGE, b.v);
}
std::uint64_t msg_enemy(int cell, int id, int len, bool partial = false)
{
    BitW b;
    b.put(cell, 12);
    b.put(R, 9);
    b.put(id, 14);
    b.put(std::min(len, 255), 8);
    b.put(partial ? 1 : 0, 1); // len is only what the sender saw
    return pack(T_ENEMY, b.v);
}
std::uint64_t msg_danger(int cell, int cause)
{
    BitW b;
    b.put(cell, 12);
    b.put(R, 9);
    b.put(cause, 3);
    return pack(T_DANGER, b.v);
}
std::uint64_t msg_sym()
{
    BitW b;
    b.put(symKnown, 2);
    return pack(T_SYM, b.v);
}
std::uint64_t msg_queen_dead(bool enemy)
{
    BitW b;
    b.put(enemy ? 1 : 0, 1);
    b.put(enemy ? enemyQueenId : queenId, 2);
    return pack(T_QUEEN_DEAD, b.v);
}
std::uint64_t msg_qspawn(int cell)
{
    BitW b;
    b.put(cell, 12);
    return pack(T_QSPAWN, b.v);
}
// The queen's bunker box (its top-left cell): guards post round it.
std::uint64_t msg_bunker(int tl)
{
    BitW b;
    b.put(tl, 12);
    b.put(R, 9);
    return pack(T_BUNKER, b.v);
}
// Phase: the highest enemy count anyone has seen, and whether we switched
// from numbers to length (newborn dragons know nothing until told).
std::uint64_t msg_phase()
{
    BitW b;
    b.put(std::min(pressureMax, 63), 6);
    b.put(switched ? 1 : 0, 1);
    b.put(R, 9);
    return pack(T_PHASE, b.v);
}
std::uint64_t msg_claim(int cell, int eta)
{
    BitW b;
    b.put(me, 14);
    b.put(cell, 12);
    b.put(std::min(eta, 255), 8);
    b.put(R, 9);
    return pack(T_CLAIM, b.v);
}

// ---------------------------------------------------------------------------
// Learning
// ---------------------------------------------------------------------------
void check_edge_symmetry(int code)
{
    for (int k = 0; k < 3; k++)
    {
        if (!(symMask & (1 << k)))
            continue;
        int const mc = mirror_edge(k, code);
        if (obsEdge[mc] == E_UNK)
            continue;
        if (obsEdge[mc] != obsEdge[code])
            symMask &= ~(1 << k);
        else
            symConfirm[k]++;
    }
}

void set_partner(int a, int b)
{
    if (a == b || edgePartner[a] == b)
        return;
    edgePartner[a] = b;
    edgePartner[b] = a;
    edgeVersion++;
    if (edgeType[a] == E_UNK) edgeType[a] = E_PORTAL;
    if (edgeType[b] == E_UNK) edgeType[b] = E_PORTAL;
    portalPairs.push_back({a, b});
}

void mirror_pair(int a, int b)
{
    if (symKnown < 0)
        return;
    int const ma = mirror_edge(symKnown, a), mb = mirror_edge(symKnown, b);
    if (edgePartner[ma] < 0 && edgePartner[mb] < 0 && ma != mb)
        set_partner(ma, mb);
}

// Record an edge. direct = seen by a dragon (us or a teammate), which is what
// symmetry testing trusts. Returns true if it taught us something new.
bool learn_edge(int code, std::uint8_t type, int pid, bool direct)
{
    bool changed = false;
    if (edgeType[code] != type)
    {
        edgeType[code] = type;
        edgeVersion++;
        changed = true;
        if (type == E_KELP)
            kelpList.push_back(code);
    }
    if (direct && obsEdge[code] != type)
    {
        obsEdge[code] = type;
        check_edge_symmetry(code);
    }
    if (type == E_PORTAL && pid >= 0 && edgePid[code] != pid)
    {
        edgePid[code] = pid;
        changed = true;
        auto& v = portalById[pid];
        if (std::find(v.begin(), v.end(), code) == v.end())
            v.push_back(code);
        if (v.size() == 2 && edgePartner[v[0]] != v[1])
        {
            set_partner(v[0], v[1]);
            mirror_pair(v[0], v[1]);
            queue_fact(msg_portal(v[0], v[1]), 85, 4); // section 5: share portal structure
        }
        else if (v.size() == 1)
            queue_fact(msg_portal_edge(code, pid), 40, 2);
    }
    if (symKnown >= 0)
    {
        int const mc = mirror_edge(symKnown, code);
        if (edgeType[mc] == E_UNK)
        {
            edgeType[mc] = type;
            edgeVersion++;
            if (type == E_KELP)
                kelpList.push_back(mc);
        }
    }
    return changed;
}

void mirror_tile(int c)
{
    if (symKnown < 0)
        return;
    int const m = mirror_cell(symKnown, c);
    if (m == c || seenRound[m] == R)
        return;
    // Mirrored tiles share one countdown, so the timing carries over exactly.
    if (nextSpawn[c] != -1 && (nextSpawn[m] == -1 || seenRound[m] < seenRound[c]))
    {
        nextSpawn[m] = nextSpawn[c];
        nextEst[m] = nextEst[c];
    }
    if (gapLo[m] == 0 && gapLo[c] > 0)
    {
        gapLo[m] = gapLo[c];
        gapHi[m] = gapHi[c];
    }
}

void note_drop(int c, int round)
{
    if (!FEEDER_TRAIL)
        return;
    for (auto const& e : dropEv)
        if (e.cell == c && std::abs(e.round - round) <= 2)
            return;
    bool nearQ = false;
    auto const q = fleet.find(queenId);
    if (!ourQueenDead && q != fleet.end() && R - q->second.round <= 20 && manhattan(q->second.cell, c) <= 6)
        nearQ = true;
    if (!ourQueenDead && me == queenId && !body.empty() && manhattan(body[0], c) <= 6)
        nearQ = true;
    dropEv.push_back({c, round, nearQ});
    if (dropEv.size() > 64)
        dropEv.erase(dropEv.begin());
}

void observe_tile(int c, bool pearl, int k)
{
    int const ns = k < 0 ? -2 : R + std::max(k, 0);
    // A reset between two sightings reveals one draw of the tile's hidden gap.
    if (obsNext[c] >= 0 && obsNext[c] <= R && ns >= 0 && ns > obsNext[c])
    {
        int const g = ns - obsNext[c];
        gapLo[c] = gapLo[c] == 0 ? g : std::min(gapLo[c], g);
        gapHi[c] = std::max(gapHi[c], g);
    }
    bool const newTiming = obsNext[c] != ns;
    bool const wasPresent = pearlState[c] == 1;
    seenRound[c] = R;
    obsNext[c] = ns;
    nextSpawn[c] = ns;
    nextEst[c] = 0;

    // Symmetry test on shared countdowns.
    for (int kk = 0; kk < 3; kk++)
    {
        if (!(symMask & (1 << kk)))
            continue;
        int const m = mirror_cell(kk, c);
        if (m == c || seenRound[m] < 0 || obsNext[m] == -1)
            continue;
        bool const neverA = ns == -2, neverB = obsNext[m] == -2;
        if (neverA != neverB)
            symMask &= ~(1 << kk);
        else if (!neverA && ns > R && obsNext[m] > R)
        {
            if (ns == obsNext[m])
                symConfirm[kk]++;
            else
                symMask &= ~(1 << kk);
        }
    }

    if (pearl)
    {
        if (!wasPresent && k < 0)
        {
            dropRound[c] = R; // a pearl on a tile that never spawns: a death drop
            note_drop(c, R);
        }
        if (!wasPresent)
        {
            queue_pearl(1, c, R, k < 0 ? 75 : 60); // death drops first: teammates come and collect
            // A fresh pearl here means the mirrored tile fired at the same moment.
            if (symKnown >= 0)
            {
                int const m = mirror_cell(symKnown, c);
                if (m != c && seenRound[m] < R && pearlState[m] == 0)
                {
                    pearlState[m] = 2;
                    pearlRound[m] = R;
                }
            }
        }
        pearlState[c] = 1;
    }
    else
    {
        if (wasPresent)
            queue_pearl(2, c, R, 35);
        pearlState[c] = 0;
    }
    pearlRound[c] = R;
    if (newTiming && ns >= 0 && ns - R <= 25)
        queue_pearl(0, c, ns, 25);
    mirror_tile(c);
}

// Tiles out of view whose countdown ran out: a pearl probably spawned there.
void update_unobserved()
{
    for (int c = 0; c < N; c++)
    {
        int const ns = nextSpawn[c];
        if (ns < 0 || ns > R || seenRound[c] >= ns)
            continue;
        if (pearlState[c] == 0)
        {
            pearlState[c] = 2;
            pearlRound[c] = ns;
        }
        if (gapLo[c] > 0)
        {
            nextSpawn[c] = ns + (gapLo[c] + gapHi[c]) / 2;
            nextEst[c] = 1;
        }
        else
            nextSpawn[c] = -1;
    }
}

void set_symmetry(int k)
{
    if (symKnown >= 0 || k < 0 || k > 2)
        return;
    symKnown = k;
    queue_fact(msg_sym(), 90, 4);
    int const codes = N * 2;
    for (int code = 0; code < codes; code++)
    {
        if (obsEdge[code] == E_UNK)
            continue;
        int const mc = mirror_edge(k, code);
        if (edgeType[mc] == E_UNK)
        {
            edgeType[mc] = obsEdge[code];
            edgeVersion++;
            if (obsEdge[code] == E_KELP)
                kelpList.push_back(mc);
        }
    }
    auto const pairs = portalPairs;
    for (auto const& [a, b] : pairs)
        mirror_pair(a, b);
    for (int c = 0; c < N; c++)
        if (seenRound[c] >= 0)
            mirror_tile(c);
}

void resolve_symmetry()
{
    if (symKnown >= 0 || symFailed)
        return;
    if (symMask == 0)
    {
        symFailed = true; // inconsistent observations: stop predicting
        return;
    }
    int bits = 0, first = -1;
    bool allConfirmed = true;
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
        {
            bits++;
            if (first < 0) first = k;
            if (symConfirm[k] < 80) allConfirmed = false;
        }
    // Several survivors with lots of evidence: the map has several symmetries,
    // and any of them predicts correctly.
    if (bits == 1 || allConfirmed)
        set_symmetry(first);
}

// ---------------------------------------------------------------------------
// Reading the turn
// ---------------------------------------------------------------------------
void read_vision(Controller& ct)
{
    std::fill(vis.begin(), vis.end(), 0);
    std::fill(occId.begin(), occId.end(), -1);
    heads.clear();
    enemyTails.clear();
    eqBodyNow.clear();
    std::unordered_map<int, int> partCount;

    for (Tile const& tile : ct.get_tiles())
    {
        int const c = cell_of(tile.get_position());
        vis[c] = 1;
        observe_tile(c, tile.has_pearl(), tile.get_pearl_time());
        for (int d = 0; d < 4; d++)
        {
            Edge const& e = tile.get_edge(dir_of(d));
            std::uint8_t const t = e.get_edge_type() == EdgeType::KELP ? E_KELP
                                 : e.get_edge_type() == EdgeType::PORTAL ? E_PORTAL
                                                                        : E_OPEN;
            learn_edge(edge_code(c, d), t, e.get_portal_id(), true);
        }
        if (DragonPart const* p = tile.get_dragon())
        {
            occId[c] = p->get_id();
            occMine[c] = p->get_team() == myTeam;
            occHead[c] = p->is_head();
            occDir[c] = dir_index(p->get_dir());
            partCount[p->get_id()]++;
            if (p->get_id() <= 1 && me > 1)
            {
                bool const ours = p->get_team() == myTeam;
                queenSeen = true;
                queenId = ours ? p->get_id() : 1 - p->get_id();
                enemyQueenId = 1 - queenId;
            }
            // A pearl can never sit under a dragon.
            pearlState[c] = 0;
        }
    }
    for (Tile const& tile : ct.get_tiles())
    {
        DragonPart const* p = tile.get_dragon();
        if (!p || !p->is_head() || p->get_id() == me)
            continue;
        heads.push_back({cell_of(tile.get_position()), p->get_id(), partCount[p->get_id()],
                         dir_index(p->get_dir()), p->get_team() == myTeam});
    }

    // Other dragons: solid by default, time-released when the whole body is visible.
    int const myHead = cell_of(ct.get_position());
    for (int c = 0; c < N; c++)
    {
        otherRel[c] = (vis[c] && occId[c] >= 0 && occId[c] != me) ? INF : 0;
        contested[c] = 0;
    }
    for (auto const& h : heads)
        for (int d = 0; d < 4; d++)
        {
            int const n = nb(h.cell, d);
            if (!(vis[n] && occId[n] >= 0))
                contested[n] = 1;
        }
    // The queen also assumes enemies use their free sprint steps.
    if (me == queenId && !QUEEN_THREAT_MAP)
        for (auto const& h : heads)
        {
            if (h.mine)
                continue;
            int const reach = std::max(1, (h.len + 3) / 4);
            if (reach <= 1)
                continue;
            for (int c = 0; c < N; c++)
                if (vis[c] && !(occId[c] >= 0) && manhattan(c, h.cell) <= reach)
                    contested[c] = 1;
        }
    for (auto& h : heads)
    {
        std::vector<int> chain{h.cell};
        for (bool grew = true; grew;)
        {
            grew = false;
            int const cur = chain.back();
            for (int d = 0; d < 4; d++)
            {
                int const n = nb(cur, d);
                if (vis[n] && occId[n] == h.id && !occHead[n] && nb(n, occDir[n]) == cur &&
                    std::find(chain.begin(), chain.end(), n) == chain.end())
                {
                    chain.push_back(n);
                    grew = true;
                    break;
                }
            }
        }
        int const m = static_cast<int>(chain.size());
        bool const partial = m != partCount[h.id] || cheb(chain.back(), myHead) > 2;
        if (!h.mine && h.id == enemyQueenId)
            eqBodyNow = chain;
        if (!h.mine)
        {
            h.partial = partial;
            h.estLen = est_enemy_len(h.id, std::max(m, partCount[h.id]), partial);
            remember_enemy_len(h.id, partial ? std::max(m, partCount[h.id]) : m, !partial);
        }
        if (partial)
            continue; // tail may continue out of view
        if (!h.mine && m >= 4)
            enemyTails.push_back(chain.back());
        for (int i = 0; i < m; i++)
            otherRel[chain[i]] = (m - 1 - i) + 3;
    }
}

// Own body: follow the facing chain from the head inside the window, then
// extend with last turn's body for the part that is out of view.
void update_body(Controller& ct)
{
    int const head = cell_of(ct.get_position());
    std::vector<int> chain{head};
    std::vector<char> inChain(N, 0);
    inChain[head] = 1;
    while (static_cast<int>(chain.size()) < L)
    {
        int const cur = chain.back();
        int found = -1;
        for (int d = 0; d < 4 && found < 0; d++)
        {
            int const n = nb(cur, d);
            if (vis[n] && occId[n] == me && !inChain[n] && !occHead[n] && nb(n, occDir[n]) == cur)
                found = n;
        }
        if (found < 0)
            break;
        chain.push_back(found);
        inChain[found] = 1;
    }
    // Replay last turn's steps from last turn's body: exact even when the
    // body is broken across a portal or the move was a sprint.
    if (!prevBody.empty())
    {
        std::vector<int> path;
        int pos = prevBody[0];
        bool ok = true;
        for (std::size_t i = 0; i < lastDirs.size(); i++)
        {
            pos = cross(pos, lastDirs[i]);
            if (pos < 0)
            {
                if (pos == -3 && i + 1 == lastDirs.size())
                    pos = head; // first trip through this portal: we now stand at its far side
                else
                {
                    ok = false;
                    break;
                }
            }
            path.push_back(pos);
        }
        if (ok && pos == head)
        {
            std::vector<int> pred(path.rbegin(), path.rend());
            pred.insert(pred.end(), prevBody.begin(), prevBody.end());
            if (static_cast<int>(pred.size()) > L)
                pred.resize(L);
            bool consistent = true;
            for (std::size_t i = 0; i < chain.size() && i < pred.size(); i++)
                consistent &= chain[i] == pred[i];
            if (consistent && pred.size() >= chain.size())
            {
                body = pred;
                return;
            }
        }
    }
    if (static_cast<int>(chain.size()) < L && !prevBody.empty())
    {
        auto const it = std::find(prevBody.begin(), prevBody.end(), chain.back());
        if (it != prevBody.end())
            for (auto j = it + 1; j != prevBody.end() && static_cast<int>(chain.size()) < L; ++j)
                if (!inChain[*j])
                {
                    chain.push_back(*j);
                    inChain[*j] = 1;
                }
    }
    body = chain;
}

void prune_knowledge()
{
    std::erase_if(enemyReps, [](EnemyRep const& e) { return R - e.round > 60; });
    std::erase_if(dangers, [](Danger const& d) { return R - d.round > 60; });
    std::erase_if(fleet, [](auto const& kv) { return R - kv.second.round > 60; });
    std::erase_if(claims, [](auto const& kv) { return R - kv.second.round > 6; });
    if (enemyReps.size() > 200)
        enemyReps.erase(enemyReps.begin(), enemyReps.begin() + (enemyReps.size() - 200));
    if (dangers.size() > 100)
        dangers.erase(dangers.begin(), dangers.begin() + (dangers.size() - 100));
}

void add_enemy(int cell, int round, int id, int len, int estLen = 0)
{
    if (id == enemyQueenId)
        lastEqSeenR = std::max(lastEqSeenR, round);
    for (auto& e : enemyReps)
        if (e.id == id && e.round <= round)
        {
            e = {cell, round, id, len, estLen};
            return;
        }
    enemyReps.push_back({cell, round, id, len, estLen});
}

// Returns true if the message taught us something (so it is worth relaying).
bool apply_message(std::uint64_t m)
{
    int type;
    std::uint64_t payload;
    if (!unpack(m, type, payload))
        return false;
    BitR b(payload);
    switch (type)
    {
    case T_STATUS: {
        int const id = b.get(14), len = b.get(10), cell = b.get(12), leader = b.get(1), round = b.get(9);
        if (id == me || cell >= N || round > R)
            return false;
        if (id <= 1 && me > 1 && !queenSeen) // only teammates speak our protocol, so this is our queen
        {
            queenId = id;
            enemyQueenId = 1 - id;
        }
        auto it = fleet.find(id);
        if (it != fleet.end() && it->second.round >= round)
            return false;
        fleet[id] = {len, cell, round, leader == 1};
        if (id == queenId)
            return true; // relay the queen's position across the fleet
        return leader == 1;
    }
    case T_PEARL: {
        bool changed = false;
        int const count = 1 + b.get(1);
        for (int i = 0; i < count; i++)
        {
            int const kind = b.get(2), cell = b.get(12), round = b.get(9);
            if (cell >= N)
                continue;
            if (kind == 0 && round >= R && (obsNext[cell] < R || obsNext[cell] == -1) && obsNext[cell] != -2)
            {
                obsNext[cell] = round;
                nextSpawn[cell] = round;
                nextEst[cell] = 0;
                mirror_tile(cell);
                changed = true;
            }
            else if (kind == 1 && pearlRound[cell] < round)
            {
                if (nextSpawn[cell] == -2 && pearlState[cell] != 1)
                    note_drop(cell, round); // a teammate saw a death drop
                pearlState[cell] = 1;
                pearlRound[cell] = round;
                changed = true;
            }
            else if (kind == 2 && pearlRound[cell] <= round && pearlState[cell] != 0)
            {
                pearlState[cell] = 0;
                pearlRound[cell] = round;
                changed = true;
            }
        }
        return changed;
    }
    case T_PORTAL: {
        int const a = b.get(13), c = b.get(13);
        if (a >= 2 * N || c >= 2 * N || edgePartner[a] == c)
            return false;
        set_partner(a, c);
        mirror_pair(a, c);
        return true;
    }
    case T_PORTAL_EDGE: {
        int const code = b.get(13), pid = b.get(20);
        if (code >= 2 * N || edgePid[code] == pid)
            return false;
        return learn_edge(code, E_PORTAL, pid, true);
    }
    case T_KELP: {
        bool changed = false;
        int const count = b.get(2);
        for (int i = 0; i < count; i++)
        {
            int const code = b.get(13);
            if (code < 2 * N && edgeType[code] != E_KELP)
                changed |= learn_edge(code, E_KELP, -1, true);
        }
        return changed;
    }
    case T_ENEMY: {
        int const cell = b.get(12), round = b.get(9), id = b.get(14), len = b.get(8);
        bool const partial = b.get(1) == 1;
        if (cell >= N || round > R)
            return false;
        if (R - round <= 2)
            remember_enemy_len(id, len, !partial);
        add_enemy(cell, round, id, len, est_enemy_len(id, len, partial));
        return R - round <= 2;
    }
    case T_DANGER: {
        int const cell = b.get(12), round = b.get(9), cause = b.get(3);
        if (cell >= N || round > R)
            return false;
        for (auto const& d : dangers)
            if (d.cell == cell && d.round == round)
                return false;
        dangers.push_back({cell, round, cause});
        return true;
    }
    case T_SYM: {
        int const k = b.get(2);
        if (symKnown >= 0 || k > 2 || !(symMask & (1 << k)))
            return false;
        set_symmetry(k);
        return true;
    }
    case T_BUNKER: {
        int const tl = b.get(12), round = b.get(9);
        if (tl >= N || round > R || round <= qBunkerR)
            return false;
        qBunkerTl = tl;
        qBunkerR = round;
        return R - round <= 3;
    }
    case T_PHASE: {
        int const pm = b.get(6), sw = b.get(1), round = b.get(9);
        if (round > R)
            return false;
        heardPressure = true;
        bool changed = pm > pressureMax || (sw == 1 && !switched);
        pressureMax = std::max(pressureMax, pm);
        if (sw == 1 && R >= SW_LO)
            switched = true;
        return changed;
    }
    case T_QSPAWN: {
        int const cell = static_cast<int>(b.get(12));
        if (ourQueenSpawn < 0 && cell < N)
        {
            ourQueenSpawn = cell;
            return true;
        }
        return false;
    }
    case T_QUEEN_DEAD: {
        bool const enemy = b.get(1) == 1;
        if (enemy && !enemyQueenDead)
        {
            enemyQueenDead = true;
            return true;
        }
        if (!enemy && !ourQueenDead && !amQueen)
        {
            ourQueenDead = true;
            return true;
        }
        return false;
    }
    case T_CLAIM: {
        int const id = b.get(14), cell = b.get(12), eta = b.get(8), round = b.get(9);
        if (id == me || cell >= N || round > R)
            return false;
        auto it = claims.find(cell);
        if (it != claims.end() && it->second.round > round)
            return false;
        claims[cell] = {id, eta, round};
        return false;
    }
    default:
        return false;
    }
}

void read_sonar(Controller& ct)
{
    if (DECOYS)
        for (std::uint64_t const m : ct.get_sonar_messages())
        {
            int t;
            std::uint64_t pl;
            if (m == 0 || unpack(m, t, pl))
                continue;
            bool dup = false;
            for (auto const& f : foreignMsgs)
                dup = dup || f.msg == m;
            if (dup)
                continue;
            foreignMsgs.push_back({m, R});
            if (foreignMsgs.size() > 32)
                foreignMsgs.erase(foreignMsgs.begin());
        }
    for (std::uint64_t const m : ct.get_sonar_messages())
        if (apply_message(m))
        {
            int type;
            std::uint64_t payload;
            int prio = 15;
            bool const typed = unpack(m, type, payload);
            if (QUEEN_POS_RELAY && typed && type == T_STATUS && me != queenId)
            {
                // The queen's position: guards and screens need it, so it goes
                // out ahead of pearl gossip while fresh.
                int const id = static_cast<int>(payload & 0x3FFF);
                int const round = static_cast<int>((payload >> 37) & 0x1FF);
                if (id == queenId && R - round <= 4)
                    prio = 45;
                else if (GIANT && ((payload >> 36) & 1) && R - round <= 4)
                    prio = 35; // leaders (the giant among them) are fed too
            }
            if (DYN_ALLOC && typed && type == T_PHASE)
                prio = 40; // the whole fleet must switch together
            if (BUNKER_GUARDS && typed && type == T_BUNKER)
                prio = 45; // guards must find her box
            if (AIM_REPORTS && me != queenId && typed && type == T_ENEMY)
            {
                // An enemy near our queen: pass it on first (and toward her).
                auto const q = fleet.find(queenId);
                int const cell = static_cast<int>(payload & 0xFFF);
                if (q != fleet.end() && R - q->second.round <= 3 && cell < N && manhattan(cell, q->second.cell) <= 14)
                    prio = 85;
            }
            queue_fact(m, prio, prio > 15 ? 1 : 2); // gossip: pass new facts along the chain
        }
    SonarEchoes const e = ct.get_sonar_echoes();
    echoEnemy = e.enemy + e.enemy_head;
}

// Section 17: a teammate that vanished from the middle of our window died.
void detect_teammate_deaths()
{
    int const head = body[0];
    std::unordered_set<int> visibleIds;
    for (int c = 0; c < N; c++)
        if (vis[c] && occId[c] >= 0)
            visibleIds.insert(occId[c]);
    for (auto const& [id, cell] : prevTeamHeads)
    {
        if (visibleIds.count(id) || cheb(cell, head) > 2)
            continue;
        int cause = 2;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, cell) <= 2)
                cause = 1;
        dangers.push_back({cell, R, cause});
        queue_fact(msg_danger(cell, cause), 75, 3);
        fleet.erase(id);
        if (id == queenId && !ourQueenDead)
        {
            ourQueenDead = true;
            queue_fact(msg_queen_dead(false), 95, 4);
        }
    }
    // The enemy queen vanished from the middle of our window: it died. Or she
    // is out of view and a death-drop pearl (on a tile that never spawns) lies
    // on her last body or within one turn's move of her head: a head-on kills
    // both, and the striker's own sonar dies with it, so watchers must tell.
    if (prevEnemyQueenCell >= 0 && !enemyQueenDead && !visibleIds.count(enemyQueenId))
    {
        int const fq = EQ_DEATH_DETECT ? std::max(1, (prevEnemyQueenLen + 3) / 4) : 1;
        bool dead = cheb(prevEnemyQueenCell, head) <= 3 - fq;
        for (int c = 0; c < N && !dead && EQ_DEATH_DETECT; c++)
        {
            if (!vis[c] || dropRound[c] != R)
                continue;
            if ((!EQ_STRICT && manhattan(c, prevEnemyQueenCell) <= fq) ||
                std::find(prevEnemyQueenBody.begin(), prevEnemyQueenBody.end(), c) != prevEnemyQueenBody.end())
                dead = true;
        }
        if (dead)
        {
            enemyQueenDead = true;
            queue_fact(msg_queen_dead(true), 95, 4);
        }
    }
    // The same from a fleet report of her (at most three rounds old: a watcher
    // that moves before the striker sees the drop only next round): a fresh
    // death drop near where she was, and she is not in our window.
    if (EQ_DEATH_DETECT && !EQ_STRICT && !enemyQueenDead && !visibleIds.count(enemyQueenId))
        for (auto const& e : enemyReps)
        {
            int const age = R - e.round;
            if (e.id != enemyQueenId || age < 1 || age > 3 || cheb(e.cell, head) > 3 + age)
                continue;
            int const rad = std::min(3, std::max(1, (e.len + 3) / 4) * age);
            if (cheb(e.cell, head) + rad <= 3)
            {
                enemyQueenDead = true; // every cell she could have reached is in view, and she is not
                queue_fact(msg_queen_dead(true), 95, 4);
                break;
            }
            for (int c = 0; c < N; c++)
                if (vis[c] && dropRound[c] == R && manhattan(c, e.cell) <= rad)
                {
                    enemyQueenDead = true;
                    queue_fact(msg_queen_dead(true), 95, 4);
                    break;
                }
        }
    prevEnemyQueenCell = -1;
    prevEnemyQueenBody = eqBodyNow;
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
        {
            prevEnemyQueenCell = h.cell;
            prevEnemyQueenLen = std::max(h.len, h.estLen);
        }
    prevTeamHeads.clear();
    for (auto const& h : heads)
        if (h.mine)
            prevTeamHeads[h.id] = h.cell;
}

void record_enemies()
{
    int shared = 0;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        add_enemy(h.cell, R, h.id, h.len, h.estLen);
        if (h.id == enemyQueenId)
            queue_fact(msg_enemy(h.cell, h.id, h.len, h.partial), 92, 2); // "one smells blood"
        else if (shared < 2)
        {
            int prio = 70;
            auto const q = fleet.find(queenId);
            if (AIM_REPORTS && me != queenId && q != fleet.end() && R - q->second.round <= 3 && manhattan(h.cell, q->second.cell) <= 14)
                prio = 88; // a threat to the queen goes out before anything else
            queue_fact(msg_enemy(h.cell, h.id, h.len, h.partial), prio, 1);
            shared++;
        }
    }
}

// ---------------------------------------------------------------------------
// Occupancy, flood fill and path finding
// ---------------------------------------------------------------------------
// Bodies are time-aware: the segment i places from the tail (tail = 0) is
// still there when the head tries to enter at step t < i + 2, because
// collision is checked before the tail moves. Other dragons get the same rule
// (plus one step of margin in case they eat) when their whole body is in view,
// and are solid otherwise.
std::vector<int> ownStamp, ownRel, wallStamp;
int stamp = 1, wstamp = 1;

void set_body(std::vector<int> const& b, int len)
{
    stamp++;
    for (int i = 0; i < static_cast<int>(b.size()); i++)
    {
        ownStamp[b[i]] = stamp;
        ownRel[b[i]] = (len - 1 - i) + 2;
    }
}

bool passable(int c, int t)
{
    if ((otherRel[c] && t < otherRel[c]) || wallStamp[c] == wstamp)
        return false;
    return ownStamp[c] != stamp || t >= ownRel[c];
}

struct FloodRes
{
    int area = 0;
    bool tail = false;
    bool frontier = false;
};

std::vector<int> bfsQ, bfsT, visStamp;
int vstamp = 1;
bool ignoreContested = false;

// Cells reachable from `start` using only edges we know are crossable.
FloodRes flood(int start, int tailCell, int cap)
{
    FloodRes r;
    vstamp++;
    bfsQ.clear();
    bfsT.clear();
    bfsQ.push_back(start);
    bfsT.push_back(0);
    visStamp[start] = vstamp;
    for (std::size_t i = 0; i < bfsQ.size() && r.area < cap; i++)
    {
        int const c = bfsQ[i], t = bfsT[i];
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2)
            {
                r.frontier = true; // unexplored water beyond: count as open
                continue;
            }
            if (n < 0 || visStamp[n] == vstamp)
                continue;
            if (!passable(n, t + 1) || (t == 0 && contested[n] && !ignoreContested))
                continue;
            visStamp[n] = vstamp;
            r.area++;
            if (n == tailCell)
                r.tail = true;
            bfsQ.push_back(n);
            bfsT.push_back(t + 1);
        }
    }
    return r;
}

// Optimistic shortest paths for planning: unknown edges count as open, kelp and
// unlocated portals as closed, known portals as shortcuts.
std::vector<int> planSteps;
std::vector<std::vector<int>> planRing;

void plan_bfs(int start, std::vector<int>& dist)
{
    std::fill(dist.begin(), dist.end(), INF);
    if (UNKNOWN_EDGE_COST > 1)
    {
        // Dial's algorithm: known edges cost 1, edges nobody has seen cost
        // more (in a maze most of them turn out to be kelp). Bodies are still
        // timed by the number of steps, not by the cost.
        int const K = UNKNOWN_EDGE_COST + 1;
        planRing.resize(K);
        for (auto& b : planRing)
            b.clear();
        dist[start] = 0;
        planSteps[start] = 0;
        planRing[0].push_back(start);
        int pending = 1;
        for (int cost = 0; pending > 0; cost++)
        {
            auto& bk = planRing[cost % K];
            for (std::size_t i = 0; i < bk.size(); i++)
            {
                int const c = bk[i];
                if (dist[c] != cost)
                    continue;
                for (int d = 0; d < 4; d++)
                {
                    int n = cross(c, d);
                    int w = 1;
                    if (n == -2)
                    {
                        n = nb(c, d);
                        w = UNKNOWN_EDGE_COST;
                    }
                    int const nt = planSteps[c] + 1;
                    if (n < 0 || cost + w >= dist[n] || !passable(n, nt))
                        continue;
                    dist[n] = cost + w;
                    planSteps[n] = nt;
                    planRing[(cost + w) % K].push_back(n);
                    pending++;
                }
            }
            pending -= static_cast<int>(bk.size());
            bk.clear();
        }
        return;
    }
    bfsQ.clear();
    bfsQ.push_back(start);
    dist[start] = 0;
    for (std::size_t i = 0; i < bfsQ.size(); i++)
    {
        int const c = bfsQ[i];
        int const t = dist[c] + 1;
        for (int d = 0; d < 4; d++)
        {
            int n = cross(c, d);
            if (n == -2)
                n = nb(c, d);
            if (n < 0 || dist[n] != INF || !passable(n, t))
                continue;
            dist[n] = t;
            bfsQ.push_back(n);
        }
    }
}

// Distance to a cell, or to its nearest neighbour if the cell itself is
// occupied (used for enemy heads and teammates).
int dist_to(std::vector<int> const& dist, int target)
{
    if (dist[target] < INF)
        return dist[target];
    int best = INF;
    for (int d = 0; d < 4; d++)
        best = std::min(best, dist[nb(target, d)] + (dist[nb(target, d)] < INF ? 1 : 0));
    return best;
}

bool fountain(int c)
{
    return FOUNTAINS && gapHi[c] > 0 && gapHi[c] <= FOUNT_GAP; // refills (almost) at once: a pocket worth mining
}
bool in_pocket(int c);

bool pearl_at(int c)
{
    return pearlState[c] == 1 && (vis[c] || R - pearlRound[c] <= 3);
}

// ---------------------------------------------------------------------------
// Fleet state: danger, transition, leaders, mode (sections 11-16)
// ---------------------------------------------------------------------------
Role role_for(int id)
{
    std::uint32_t const h = static_cast<std::uint32_t>(id) * 2654435761u;
    int const r = static_cast<int>((h >> 16) % 10);
    if (r < 3) return ROLE_COMBAT;
    if (r < 4) return ROLE_PORTAL;
    if (r < 6) return ROLE_EXPLORER;
    return ROLE_COLLECTOR;
}

// Fleet size the expansion phase aims for.
int expansion_target() { return std::min(unitLimit - QUEEN_SLOTS - 1, std::max(4, N / (DYN_ALLOC ? DYN_DENSITY : EXP_DENSITY))); }
// Growth splits allowed: before the switch round (dynamic) or EXP_END.
bool growth_open() { return DYN_ALLOC ? !switched : R < EXP_END; }
// Fleet floor once dragons feed the queen: more enemies, more dragons kept.
int cons_floor() { return DYN_ALLOC ? 10 + std::min(10, pressureMax / 3) : CONSOL_MIN_UNITS; }

void compute_fleet_state()
{
    int const head = body[0];
    std::unordered_set<int> recent;
    for (auto const& e : enemyReps)
        if (R - e.round <= 60)
            recent.insert(e.id);
    distinctEnemies = static_cast<int>(recent.size());
    // Dynamic allocation: the more enemies we meet, the longer numbers matter
    // and the later we turn them into length. Sticky once switched.
    if (DYN_ALLOC)
    {
        if (firstR < 0)
            firstR = R;
        pressureMax = std::max(pressureMax, distinctEnemies);
        switchRound = SWITCH_FIXED > 0 ? SWITCH_FIXED : SW_LO + (SW_HI - SW_LO) * std::min(pressureMax, SW_E_HI) / SW_E_HI;
        bool const informed = heardPressure || R - firstR >= 8 || firstR <= 1;
        if (!switched && R >= switchRound && informed)
        {
            switched = true;
            queue_fact(msg_phase(), 70, 3);
        }
        if (R % 3 == me % 3)
            queue_fact(msg_phase(), 50, 1); // often: newborns and the queen hear little
    }
    enemyEst = distinctEnemies;
    if (ENEMY_EST && distinctEnemies > 0)
    {
        // We hear of the enemies our fleet happens to see; a swarm is mostly
        // out of view. Scale by the share of the map our dragons watch.
        double const cover = std::max(EST_MIN_COVER_X100 / 100.0, std::min(1.0, units * static_cast<double>(EST_VIEW_CELLS) / N));
        enemyEst = std::max(distinctEnemies, static_cast<int>(distinctEnemies / cover + 0.5));
    }
    outnumbered = enemyEst > units;
    lateSafe = LATE_SAFE && R >= 500 - LATE_SAFE_ROUNDS;
    survivalMode = SURVIVAL && (units <= SURV_UNITS || (distinctEnemies >= SURV_RATIO_MIN && units * SURV_RATIO < distinctEnemies) || lateSafe);

    // Section 12: more opponents around -> keep the big fleet for longer.
    int const d = distinctEnemies;
    int const thr = (d >= 12 ? 400 : d >= 7 ? 360 : d >= 3 ? 310 : d >= 1 ? 250 : 200) - TRANS_SHIFT;
    if (!transitioned)
        transitionRound = thr;
    if (!transitioned && R >= transitionRound)
        transitioned = true;

    localThreat = 0;
    std::unordered_set<int> counted;
    for (auto const& h : heads)
        if (!h.mine)
        {
            localThreat++;
            counted.insert(h.id);
        }
    for (auto const& e : enemyReps)
        if (R - e.round <= 6 && !counted.count(e.id) && manhattan(e.cell, head) <= 8)
        {
            localThreat++;
            counted.insert(e.id);
        }
    if (echoEnemy >= 2)
        localThreat++;

    // P2: the home queen's leash follows the threat around her. Contact pulls
    // her home; quiet lets her eat further out (unlimited in the opening).
    if (units >= ADAPT_FLEET)
        fleetEstablished = true;
    queenOpening = QUEEN_ADAPT && !fleetEstablished && R < ADAPT_OPEN_END;
    queenLeash = QUEEN_LEASH;
    if (QUEEN_ADAPT && amQueen)
    {
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= THREAT_RADIUS)
                lastThreatR = R;
        for (auto const& e : enemyReps)
            if (R - e.round <= 3 && manhattan(e.cell, head) <= THREAT_RADIUS)
                lastThreatR = R;
        int const quiet = R - lastThreatR - QUIET_ROUNDS;
        if (quiet >= 0 && queenOpening)
            queenLeash = INF;
        else if (quiet >= 0 && R < ADAPT_END_ROUND)
            queenLeash = std::min(LEASH_MAX, QUEEN_LEASH + quiet / 2);
    }

    // Expansion (numbers first): lasts until the fleet first reaches its
    // target. The queen splits too until QUEEN_GROW_ROUND; if the enemy queen
    // is still alive by then she grows instead (once it is dead, staying alive
    // is all that counts, so she keeps splitting while there is room).
    if (EXPANSION)
    {
        if (expansionPhase && (!growth_open() || units >= expansion_target()))
            expansionPhase = false;
        queenSplits = (DYN_ALLOC ? !switched : R < QUEEN_GROW_ROUND) || enemyQueenDead;
    }
    else
        expansionPhase = queenSplits = false;

    // Hunters: a fixed share of the fleet (by id) goes after the enemy queen
    // once the opening is over. The queen and the leaders never hunt.
    {
        std::uint32_t const hh = static_cast<std::uint32_t>(me) * 2246822519u + 3266489917u;
        bool const active = HUNTERS && !(enemyQueenDead && !ourQueenDead) && (R >= HUNT_START || !expansionPhase) && R >= OPEN_FARM_ROUND &&
                            !(HUNT_GATE && (survivalMode || outnumbered) && !ourQueenDead);
        int const slot = static_cast<int>((hh >> 13) % 10);
        hunterSlot = !amQueen && slot < HUNTER_SHARE;
        isHunter = active && !amQueen && !isLeader && slot < HUNTER_SHARE;
        // Guards: their own slice of the fleet; once the enemy queen is dead
        // the hunters come home too.
        isGuard = GUARDS && !amQueen && !isLeader && !ourQueenDead && !isHunter &&
                  ((slot >= HUNTER_SHARE && slot < HUNTER_SHARE + GUARD_SHARE) || (enemyQueenDead && slot < HUNTER_SHARE)) &&
                  (GUARD_MIN_UNITS == 0 || enemyQueenDead || (units >= GUARD_MIN_UNITS && !expansionPhase)) && units > GUARD_FLEET_MIN &&
                  (R >= OPEN_FARM_ROUND || enemyQueenDead);
        isScreen = SCREENS && isGuard &&
                   (slot == HUNTER_SHARE || (SCREENS_MORE && units < 20 && slot == HUNTER_SHARE + 1 && (me & 1)));
        isDefender = OWN_HALF && !amQueen && !isLeader && !isGuard && !isHunter && slot == DEFENDER_SLOT && R >= HUNT_START;
    }

    // Too many leaders (they could not hear each other at election time):
    // step down if enough longer leaders are known.
    amQueen = me == queenId;
    if (amQueen)
        isLeader = true; // the queen leads for the whole game
    else if (isLeader)
    {
        int longer = 0;
        for (auto const& [id, s] : fleet)
            if (id != me && s.leader && R - s.round <= 30 && (s.len > L || (s.len == L && id < me)))
                longer++;
        if (longer >= LEADER_COUNT)
            isLeader = false;
    }

    // Leader election (section 11): the longest dragons we know of.
    if (transitioned && !expansionPhase && !isLeader && L >= 3)
    {
        int freshLeaders = 0;
        std::vector<std::pair<int, int>> ranking{{-L, me}};
        for (auto const& [id, s] : fleet)
        {
            if (id == me || R - s.round > 30)
                continue;
            if (s.leader)
                freshLeaders++;
            else
                ranking.push_back({-s.len, id});
        }
        int const slots = LEADER_COUNT - freshLeaders;
        std::sort(ranking.begin(), ranking.end());
        for (int i = 0; i < slots && i < static_cast<int>(ranking.size()); i++)
            if (ranking[i].second == me)
                isLeader = true;
    }

    // Heir: once the enemy queen is dead ours only has to live, and the
    // longest of the rest grows (insurance for the longest-dragon tiebreak).
    isHeir = false;
    if (HEIR && enemyQueenDead && !ourQueenDead && !amQueen)
    {
        isHeir = true;
        for (auto const& [id, s] : fleet)
            if (id != me && id != queenId && R - s.round <= 10 && (s.len > L || (s.len == L && id < me)))
                isHeir = false;
        if (isHeir)
            isLeader = true;
    }

    // P5 giant: from GIANT_ROUND the longest of the rest stops splitting and
    // grows on every map, so the queen is never our only long dragon.
    isGiant = false;
    if (GIANT_ALL && R >= GIANT_ROUND && !amQueen && !ourQueenDead && L >= GIANT_MIN_LEN)
    {
        isGiant = true;
        for (auto const& [id, s] : fleet)
            if (id != me && id != queenId && R - s.round <= 10 && (s.len > L || (s.len == L && id < me)))
                isGiant = false;
        if (isGiant)
            isLeader = true;
    }

    // Big open map late in the game: length decides the tiebreaks there and
    // long dragons do not trap themselves, so the fleet turns into length.
    // Small or kelp-dense maps keep their numbers.
    openMap = consolidate = false;
    if (N >= OPEN_MIN_CELLS)
    {
        int known = 0, kelp = 0;
        for (int code = 0; code < 2 * N; code++)
            if (edgeType[code] != E_UNK)
            {
                known++;
                kelp += edgeType[code] == E_KELP;
            }
        openMap = known > 0 && kelp <= OPEN_KELP_MAX * known;
        consolidate = CONSOLIDATE && openMap && R >= CONSOL_ROUND;
    }
    if (DYN_ALLOC)
        consolidate = switched; // every map: after the switch, length
    // SIZE_CAP_PLAN: numbers (no dragon of length 4+) until round X, then length on open maps.
    if (SIZE_CAP_PLAN)
    {
        capOpen = openMap;
        if (CAP_FIX)
        {
            // The kelp share of the edges seen so far can cross the threshold back and forth;
            // a flip after the cap ended would turn it on again over grown dragons.
            if (capOpenLock < 0 && R >= CAP_OPEN_LOCK)
                capOpenLock = openMap;
            if (capOpenLock >= 0)
                capOpen = capOpenLock;
        }
        if (!capOpen && outnumbered && R < (CAP_FIX ? CAP_LATCH_BY : CAP_SMALL_END))
            capSmallLatched = true; // small / congested map and outnumbered: numbers until CAP_SMALL_END
        capRound = capOpen ? std::clamp(switchRound, CAP_X_MIN, CAP_X_MAX) : (capSmallLatched ? CAP_SMALL_END : 0);
        capMode = R < capRound;
        if (capMode)
            consolidate = false;
        else if (capOpen)
            consolidate = true; // after X: no optional splits, the dragons grow
    }

    // Majority on a smaller map: hunt them down, win by elimination.
    majorityHunt = MAJ_HUNT && N <= SMALL_MAP_CELLS && !survivalMode && distinctEnemies >= 1 && units >= MAJ_MIN_UNITS &&
                   units * 10 >= MAJ_RATIO_X10 * enemyEst;

    // Modes (sections 13-15).
    bool enemyNearby = false;
    for (auto const& e : enemyReps)
        if (R - e.round <= 30 && manhattan(e.cell, head) <= 12)
            enemyNearby = true;
    if (localThreat >= 2 || (localThreat >= 1 && role == ROLE_COMBAT))
        mode = MODE_COMBAT;
    else if (transitioned && !enemyNearby && localThreat == 0)
        mode = MODE_FARM;
    else if (transitioned)
        mode = MODE_PRESERVE;
    else
        mode = MODE_NORMAL;
}

// ---------------------------------------------------------------------------
// Target selection (sections 6-10, 18)
// ---------------------------------------------------------------------------
struct PearlCand
{
    int cell, appear;
    double conf;
};

std::vector<int> distHead, distTmp;
std::vector<int> skipPearl; // round until which this dragon ignores the pearl (only reachable at a loss)

// Our half: nearer our queen's start than its mirror image (theirs).
int enemy_spawn_guess()
{
    if (ourQueenSpawn < 0)
        return -1;
    if (symKnown >= 0)
        return mirror_cell(symKnown, ourQueenSpawn);
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
            return mirror_cell(k, ourQueenSpawn);
    return -1;
}
int halfMargin(int c) // > 0 on our side, < 0 on theirs
{
    int const es = enemy_spawn_guess();
    if (es < 0 || es == ourQueenSpawn)
        return 0;
    return manhattan(c, es) - manhattan(c, ourQueenSpawn);
}

int est_enemy_units()
{
    return std::max(enemyEst, 1);
}

// Implicit, communication-light assignment: every dragon solves the same
// greedy matching of known teammates to known pearls, so dragons split up
// instead of all chasing one pearl (sections 6, 7, 9).
int choose_pearl(std::vector<PearlCand> const& pearls)
{
    struct Agent
    {
        int id, cell, fixedEta;
        bool leader;
    };
    std::vector<Agent> agents{{me, body[0], -1, isLeader}};
    std::unordered_set<int> seen{me};
    for (auto const& h : heads)
        if (h.mine && !seen.count(h.id))
        {
            seen.insert(h.id);
            auto it = fleet.find(h.id);
            agents.push_back({h.id, h.cell, -1, it != fleet.end() && it->second.leader});
        }
    for (auto const& [id, s] : fleet)
        if (!seen.count(id) && R - s.round <= 4 && agents.size() < 24)
        {
            seen.insert(id);
            agents.push_back({id, s.cell, -1, s.leader});
        }

    struct Pair
    {
        double cost;
        int agent, pearl;
    };
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(pearls.size()); pi++)
    {
        auto const& p = pearls[pi];
        auto const cl = claims.find(p.cell);
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            auto const& a = agents[ai];
            int eta;
            if (a.id == me)
                eta = distHead[p.cell];
            else if (cl != claims.end() && cl->second.id == a.id)
                eta = std::max(0, cl->second.eta - (R - cl->second.round));
            else
                eta = manhattan(a.cell, p.cell);
            if (eta >= INF)
                continue;
            int const wait = p.appear - R;
            // Future spawn: travel plus half the time spent waiting there.
            double cost = (CAMP_JOB && wait > 0 ? eta + 0.5 * std::max(0, wait - eta) : std::max(eta, wait)) + 1.0;
            cost /= p.conf;
            if (a.leader)
                cost *= a.id == queenId ? (EXPANSION && queenSplits ? 1.0 : 0.3) : 0.6; // feed the queen (once she grows), then the leaders
            pairs.push_back({cost + a.id * 1e-7, ai, pi});
        }
        // A teammate we cannot see but who claimed this pearl still counts.
        if (cl != claims.end() && !seen.count(cl->second.id))
        {
            double const cost = std::max(0, cl->second.eta - (R - cl->second.round)) + 1.0;
            pairs.push_back({cost / p.conf, -1, pi});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](Pair const& a, Pair const& b) { return a.cost < b.cost; });
    std::vector<char> agentUsed(agents.size(), 0), pearlUsed(pearls.size(), 0);
    for (auto const& pr : pairs)
    {
        if (pearlUsed[pr.pearl] || (pr.agent >= 0 && agentUsed[pr.agent]))
            continue;
        pearlUsed[pr.pearl] = 1;
        if (pr.agent < 0)
            continue;
        agentUsed[pr.agent] = 1;
        if (agents[pr.agent].id == me)
            return pr.pearl;
    }
    return -1;
}

extern std::vector<int> qThreat; // the queen's threat map (below)

// P7: how pearl-rich the area round each cell is (expected pearls per round
// from known spawn tiles within RV_RAD, fountains most), normalised to [0, 1].
// Recomputed every 5 rounds.
std::vector<float> regionVal;
int regionValRound = -100;
double rv_size_factor() { return std::clamp(static_cast<double>(N) / RV_FULL_CELLS, 0.4, 1.0); }
void update_region_value()
{
    if (R - regionValRound < 5 && static_cast<int>(regionVal.size()) == N)
        return;
    regionValRound = R;
    regionVal.assign(N, 0.0f);
    for (int t = 0; t < N; t++)
    {
        if (!(nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0))
            continue;
        double const rate = gapHi[t] > 0 ? 1.0 / std::max(3, gapHi[t]) : 1.0 / 25.0;
        int const tx = t % W, ty = t / W;
        for (int dy = -RV_RAD; dy <= RV_RAD; dy++)
            for (int dx = -RV_RAD + std::abs(dy); dx <= RV_RAD - std::abs(dy); dx++)
            {
                int const x = tx + dx, y = ty + dy;
                if (x < 0 || y < 0 || x >= W || y >= H)
                    continue;
                regionVal[y * W + x] += static_cast<float>(rate * (RV_RAD + 1 - std::abs(dx) - std::abs(dy)) / (RV_RAD + 1));
            }
    }
    float mx = 0;
    for (float v : regionVal)
        mx = std::max(mx, v);
    if (mx > 0)
        for (float& v : regionVal)
            v /= mx;
}
double rv_norm(int c) { return REGION_VALUE && static_cast<int>(regionVal.size()) == N ? regionVal[c] : 0.0; }

std::vector<PearlCand> gather_pearls()
{
    if (REGION_VALUE && !amQueen)
        update_region_value();
    std::vector<PearlCand> out;
    for (int c = 0; c < N; c++)
    {
        if (vis[c] && occId[c] >= 0)
            continue;
        double conf = 0;
        int appear = R;
        if (pearlState[c] == 1)
            conf = vis[c] ? 1.0 : std::max(0.2, 0.9 - 0.02 * (R - pearlRound[c]));
        else if (pearlState[c] == 2)
            conf = std::max(0.15, 0.55 - 0.01 * (R - pearlRound[c]));
        else if (nextSpawn[c] >= R &&
                 nextSpawn[c] - R <= (CAMP_JOB && !amQueen ? (distHead[c] < INF ? distHead[c] : 0) + CAMP_LEAD
                                               : std::max(25, (distHead[c] < INF ? distHead[c] : 0) + PREPOS_SLACK)))
        {
            conf = nextEst[c] ? 0.35 : CAMP_CONF;
            appear = nextSpawn[c];
        }
        if (conf <= 0 || distHead[c] >= INF || skipPearl[c] > R)
            continue;
        int const eta = distHead[c];
        if (eta > 60)
            continue;
        // Contested: an enemy head is clearly closer.
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, c) + 1 < eta)
                conf *= (AGGRO_EXPANSION && (expansionPhase || (SIZE_CAP_PLAN && capMode)) && !amQueen) ? 0.8 : 0.35;
        if (OWN_HALF && halfMargin(c) > 0)
            conf *= OWN_HALF_BONUS; // ours first: raiders feed on what we leave
        // v2.14: the queen keeps to her home ground, away from enemy heads, and
        // never goes into a pocket (a dead end is where escorts finish her).
        bool const herDrop = FEED_FIX && amQueen && eta <= FEED_EAT_DIST && pearlState[c] == 1 && qThreat[c] >= INF;
        if (amQueen && QUEEN_HOME && !queenStarvingG && !herDrop)
        {
            if (ourQueenSpawn >= 0 && manhattan(c, ourQueenSpawn) > queenLeash)
                continue;
            bool clear = true;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, c) <= QUEEN_PEARL_CLEAR)
                    clear = false;
            if (!clear || in_pocket(c))
                continue;
        }
        // Pearls right by the queen are hers: teammates going for them box her in.
        if (QUEEN_HOME && !amQueen)
        {
            bool byQueen = false;
            for (auto const& h : heads)
                if (h.mine && h.id == queenId && manhattan(h.cell, c) <= (FEED_FIX && R >= FEED_KEEP_ROUND ? FEED_KEEP_RADIUS : 3))
                    byQueen = true; // feeders die within 3 of her: their drops are for her
            if (byQueen)
                continue;
        }
        // A fountain pocket takes one miner at a time (two in a 1-wide dead
        // end block each other); a teammate already closer has it.
        if (!amQueen && fountain(c))
        {
            bool taken = false;
            for (auto const& h : heads)
                if (h.mine && h.id != me && manhattan(h.cell, c) <= 3 && manhattan(h.cell, c) < eta)
                    taken = true;
            if (taken)
                continue;
            conf *= 1.2; // renewable: worth the trip
        }
        // P7: a pearl in a rich area is worth more than a lone one at the end
        // of a corridor (the dragon stays among more pearls afterwards).
        if (REGION_VALUE && !amQueen)
            conf *= 1.0 + (RV_PEARL_W + (SIZE_CAP_PLAN && capOpen && capRound > 0 && R >= capRound ? CAP_RV_PEARL_W : 0.0)) * rv_size_factor() * rv_norm(c);
        // In survival mode she does not go near them at all.
        if (amQueen && survivalMode)
        {
            bool nearEnemy = false;
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, c) <= 8)
                    nearEnemy = true;
            if (nearEnemy)
                continue;
        }
        // The queen leaves pearls near recently reported enemies to others.
        if (amQueen)
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, c) <= 6)
                {
                    conf *= 0.25;
                    break;
                }
        // Dangerous area reported by the fleet.
        for (auto const& dg : dangers)
            if (dg.cause == 1 && R - dg.round <= 20 && manhattan(dg.cell, c) <= 3)
                conf *= isLeader ? 0.3 : 0.7;
        out.push_back({c, appear, conf});
    }
    if (out.size() > 60)
    {
        std::nth_element(out.begin(), out.begin() + 60, out.end(), [](PearlCand const& a, PearlCand const& b) {
            return std::max(distHead[a.cell], a.appear - R) / a.conf < std::max(distHead[b.cell], b.appear - R) / b.conf;
        });
        out.resize(60);
    }
    // Big timed prizes: tiles spawning together in a cluster draw several
    // campers (one per tile through the matching); a small bonus each.
    if (CAMP_JOB)
        for (auto& pc : out)
        {
            if (pc.appear <= R)
                continue;
            int mates = 0;
            for (auto const& o : out)
                if (&o != &pc && o.appear > R && std::abs(o.appear - pc.appear) <= 3 && manhattan(o.cell, pc.cell) <= 3)
                    mates++;
            pc.conf *= 1.0 + 0.1 * std::min(mates, 3);
        }
    return out;
}

// Section 9: spread out. Unseen cells first, then stale ones, away from teammates.
int choose_explore_goal()
{
    bool const valid = exploreGoal >= 0 && R - exploreSetRound < 40 && distHead[exploreGoal] < INF &&
                       exploreGoal != body[0] && (seenRound[exploreGoal] < exploreSetRound);
    if (valid)
        return exploreGoal;
    std::vector<int> teamCells;
    for (auto const& h : heads)
        if (h.mine)
            teamCells.push_back(h.cell);
    for (auto const& [id, s] : fleet)
        if (R - s.round <= 10)
            teamCells.push_back(s.cell);

    int best = -1;
    double bestScore = 1e18;
    for (int pass = amQueen ? 1 : 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < 48; i++)
        {
            int const c = static_cast<int>(rng() % static_cast<unsigned>(N));
            bool const unseen = seenRound[c] < 0;
            if (pass == 0 && !unseen)
                continue;
            if (pass == 1 && R - seenRound[c] < 25)
                continue;
            int const dh = distHead[c];
            if (dh >= INF || dh < 4)
                continue;
            int crowd = 0;
            for (int tc : teamCells)
                if (manhattan(tc, c) < 8)
                    crowd++;
            double score = dh + 25.0 * crowd;
            if (REGION_VALUE && !amQueen && R < RV_EXPLORE_END)
            {
                // P7: the opening heads for the middle of the map, where both
                // sides meet and pearl fields usually are.
                int const cx = std::abs(c % W - W / 2), cy = std::abs(c / W - H / 2);
                double const mid = std::max(0.0, 1.0 - static_cast<double>(cx + cy) / std::max(1, (W + H) / 4));
                score -= RV_EXPLORE_W * rv_size_factor() * mid;
            }
            if (role == ROLE_EXPLORER)
                score -= 0.3 * dh; // explorers push further out
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 30 && manhattan(dg.cell, c) <= 4)
                    score += isLeader ? 200 : 40;
            if (score < bestScore)
            {
                bestScore = score;
                best = c;
            }
        }
    exploreGoal = best;
    exploreSetRound = R;
    return best;
}

// Nearest spawn tile no teammate is already sitting by; sooner spawns first.
int choose_camp_goal(bool ownHalf = false)
{
    std::vector<int> mates;
    for (auto const& h : heads)
        if (h.mine && h.id != me)
            mates.push_back(h.cell);
    for (auto const& [id, s] : fleet)
        if (id != me && R - s.round <= 6)
            mates.push_back(s.cell);
    int best = -1;
    double bestScore = 1e18;
    for (int c = 0; c < N; c++)
    {
        if (nextSpawn[c] < 0 || distHead[c] >= 30 || skipPearl[c] > R)
            continue;
        bool taken = false;
        for (int m : mates)
            if (manhattan(m, c) <= 2)
                taken = true;
        if (taken)
            continue;
        double const wait = nextSpawn[c] >= R ? nextSpawn[c] - R : 15; // spawned earlier: next time unknown
        double score = distHead[c] + 0.5 * std::max(0.0, wait - distHead[c]);
        if (REGION_VALUE && !amQueen)
            score -= (RV_CAMP_W + (SIZE_CAP_PLAN && capOpen && capRound > 0 && R >= capRound ? CAP_RV_CAMP_W : 0.0)) * rv_size_factor() * rv_norm(c); // P7: rich areas first
        if (ownHalf)
        {
            int const hm = halfMargin(c);
            if (hm <= 0)
                continue;
            score += 0.7 * hm; // near the middle: where raiders come through
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

// Section 5: nearest portal edge whose far side is still unknown.
int choose_portal_goal(int& portalDir)
{
    int best = -1, bestD = 30;
    portalDir = -1;
    for (int c = 0; c < N; c++)
    {
        if (distHead[c] >= bestD)
            continue;
        for (int d = 0; d < 4; d++)
            if (cross(c, d) == -3)
            {
                best = c;
                bestD = distHead[c];
                portalDir = d;
                break;
            }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------
struct Cand
{
    int d = 0, dest = -1;
    bool legal = false, blind = false, attack = false, eat = false;
    bool ok1 = false, ok2 = false;
    int area = 0;
    double dive = -1e9; // net segments won by entering a dead end (+0.5 if we get out whole); non-queens
    bool surv = false;  // queen: some way on through known or unexplored water
    int dt = INF;       // path distance to the target after this move
    bool noLoop = false; // queen (length <= 3): no free 2x2 reachable afterwards
    double score = -1e18;
    std::vector<int> nb1;
};

bool is_adjacent_enemy_head(int cell)
{
    for (auto const& h : heads)
        if (!h.mine && manhattan(h.cell, cell) <= 1)
            return true;
    return false;
}

// Can a child made of the rear `childLen` segments get out? Its head is my
// tail and it moves later this round; my front part stays where it is.
bool child_can_escape(int childLen, bool growth = false)
{
    if (static_cast<int>(body.size()) < L)
        return true; // tail is out of view, well away from whatever traps the head
    std::vector<int> child(body.rbegin(), body.rbegin() + childLen);
    wstamp++;
    for (int i = 0; i < L - childLen; i++)
        wallStamp[body[i]] = wstamp;
    set_body(child, childLen);
    int const need = growth && SPLIT_ROOM ? std::max(6, 2 * childLen + 4) : std::min(childLen + 1, 6);
    FloodRes const f = flood(child[0], child.back(), std::max(childLen + 8, need + 2));
    wstamp++;
    set_body(body, L);
    // A growth split is optional: its child must have room to live, not just
    // to take a step (children born into pockets died within a few rounds).
    return (f.tail && !(growth && SPLIT_ROOM)) || f.area >= need;
}

// Numbers are the goal, but a crowded map kills small dragons for nothing.
bool room_for_more()
{
    if (SIZE_CAP_PLAN && capMode)
        return units < unitLimit - QUEEN_SLOTS; // the plan: fill up to the unit limit before X
    if (EXPANSION && growth_open())
        return units < expansion_target();
    return units < std::min(unitLimit, std::max(2, N / SPLIT_DENSITY));
}

// The engine refuses every split once the team holds its unit limit. The
// queen's survival split must never be refused, so the others leave her
// QUEEN_SLOTS free (several dragons can split in the same round).
bool split_allowed(Controller& ct, int child)
{
    return ct.can_split(child) && (amQueen || units < unitLimit - QUEEN_SLOTS);
}

void act_move(Controller& ct, int d, char const* why)
{
    ct.make_move(dir_of(d));
    note = why;
    prevBody = body;
    lastDirs = {d};
}

void act_split(Controller& ct, int child, char const* why)
{
    ct.do_split(child);
    note = why;
    prevBody = body;
    lastDirs.clear();
}

// ---------------------------------------------------------------------------
// Queen survival search
// ---------------------------------------------------------------------------
// Area alone cannot tell a dead-end corridor from open water, and a snake
// cannot turn round in a corridor. So for the queen we look for any path of
// `need` further moves through known water. Body cells (set_body) free up
// over time; cells the head passes through stay occupied for `len` moves.
std::vector<int> pathStamp, pathTime;
int pstamp = 1, survNodes = 0;

int survCap = SURV_NODE_CAP; // node budget of the current survival search
bool survive_rec(int h, int t, int need, int len)
{
    if (t >= need || ++survNodes > survCap)
        return true; // long enough (or out of budget: assume fine)
    for (int d = 0; d < 4; d++)
    {
        int n = cross(h, d);
        if (n == -2)
        {
            if (SURV_UNKNOWN_SAFE)
                return true;
            // Unexplored edge: assume open water, but our own body (often out
            // of view on a long queen) and other dragons still block.
            n = nb(h, d);
        }
        if (n < 0)
            continue;
        int const nt = t + 1;
        if (!passable(n, nt) || (nt == 1 && contested[n]))
            continue;
        if (pathStamp[n] == pstamp && nt < pathTime[n] + len + 1)
            continue;
        int const saveS = pathStamp[n], saveT = pathTime[n];
        pathStamp[n] = pstamp;
        pathTime[n] = nt;
        bool const ok = survive_rec(n, nt, need, len);
        pathStamp[n] = saveS;
        pathTime[n] = saveT;
        if (ok)
            return true;
    }
    return false;
}

// b is the body after the move (head first), with its length.
bool queen_can_survive(std::vector<int> const& b, int len, int nodeCap = SURV_NODE_CAP)
{
    set_body(b, len);
    pstamp++;
    survNodes = 0;
    survCap = nodeCap;
    pathStamp[b[0]] = pstamp;
    pathTime[b[0]] = 0;
    return survive_rec(b[0], 0, std::min(2 * len + 6, 50), len);
}

// Body after a sequence of steps (free steps only), or empty if a step is illegal.
std::vector<int> simulate_steps(std::vector<int> b, int len, std::vector<int> const& dirs, int& outLen)
{
    std::vector<int> eaten;
    for (int d : dirs)
    {
        int const n = cross(b[0], d);
        if (n < 0)
            return {};
        bool const eat = pearl_at(n) && std::find(eaten.begin(), eaten.end(), n) == eaten.end();
        if (eat)
            eaten.push_back(n);
        b.insert(b.begin(), n);
        if (eat)
            len++;
        else if (static_cast<int>(b.size()) > len)
            b.pop_back();
    }
    outLen = len;
    return b;
}

// ---------------------------------------------------------------------------
// Dead-end dives
// ---------------------------------------------------------------------------
// A dead end that holds pearls can still be worth entering when the pearls pay
// for the way out. At the bottom the survival split leaves the front two
// segments behind and the rest leaves as a child, whose head is our old tail
// outside the dead end. This searches our own moves through known water;
// other dragons keep their bodies. Value = net segments won.
int diveNodes = 0, diveCap = DIVE_NODE_CAP;

// After a split of all but the front 2 segments, can the child get away?
bool split_child_escapes(std::vector<int> const& b, int len)
{
    if (static_cast<int>(b.size()) < len)
        return true; // tail out of view, well away from the dead end
    int const childLen = len - 2;
    std::vector<int> child(b.rbegin(), b.rbegin() + childLen);
    wstamp++;
    for (int i = 0; i < len - childLen; i++)
        wallStamp[b[i]] = wstamp;
    set_body(child, childLen);
    FloodRes const f = flood(child[0], child.back(), childLen + 8);
    wstamp++;
    return f.tail || f.area >= std::min(childLen + 1, 6);
}

double dive_rec(std::vector<int> const& b, int len, int depth, int eaten, std::vector<int>& eatenCells)
{
    int const h = b[0];
    set_body(b, len);
    int const tailCell = static_cast<int>(b.size()) >= len ? b.back() : -1;
    FloodRes const f = flood(h, tailCell, 2 * len + 4);
    if (f.tail || f.area + (f.frontier ? len : 0) >= 2 * len + 4 || (FRONTIER_OPEN && !isLeader && f.frontier))
        return eaten + 0.5; // back in open water: everything eaten is profit, nobody lost
    if (depth >= DIVE_DEPTH || ++diveNodes > diveCap)
        return eaten - 3; // unresolved: assume the split and a bit more
    double best = -1e9;
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(h, d);
        if (n < 0)
            continue;
        set_body(b, len);
        if (!passable(n, 1))
            continue;
        bool const fount = fountain(n) && !(vis[n] && occId[n] >= 0 && occId[n] != me); // refills before we get there
        bool const eat = (pearl_at(n) || fount) && std::find(eatenCells.begin(), eatenCells.end(), n) == eatenCells.end();
        std::vector<int> nb2;
        nb2.reserve(b.size() + 1);
        nb2.push_back(n);
        nb2.insert(nb2.end(), b.begin(), b.end());
        int const nl = len + (eat ? 1 : 0);
        if (!eat && static_cast<int>(b.size()) >= len)
            nb2.pop_back();
        if (eat)
            eatenCells.push_back(n);
        best = std::max(best, dive_rec(nb2, nl, depth + 1, eaten + (eat ? 1 : 0), eatenCells));
        if (eat)
            eatenCells.pop_back();
    }
    if (best > -1e9)
        return best;
    // Stuck: the survival split saves all but the front two segments.
    if (len >= 4 && units < unitLimit && split_child_escapes(b, len))
        return eaten - 2;
    return eaten - len;
}

// ---------------------------------------------------------------------------
// Sprints: the first ceil(L/4) steps of a turn are free.
// ---------------------------------------------------------------------------
int free_steps() { return (L + 3) / 4; }
int enemy_reach(HeadInfo const& h) { return std::max(1, (h.len + 3) / 4); }

struct SprintPlan
{
    std::vector<int> dirs;
    double value = -1e18;
    bool kill = false;
};

std::vector<int> distGoal;

// Danger of ending a turn on cell n: next to a teammate head, or where an
// enemy head can reach us (leaders assume the enemy sprints its free steps).
// Queen threat map: the fewest paid sprint steps an enemy needs to put its
// head on a cell (0 = within its free steps, INF = out of reach). A head-on
// kills both dragons whatever their lengths, and a dragon may sprint past its
// free steps by paying one segment per step down to length 2.
std::vector<int> qThreat, qThreatVis, thStamp, thDist, thQ;
std::vector<int> qThreatCnt, qMargin; // enemies able to reach a cell; steps beyond the nearest reach
std::vector<int> qThreat2;            // P3c: < INF where some enemy can put its head within two turns
std::vector<int> l2Stamp, l2Dist, l2Q;
int l2S = 1;
bool look2Active = false;             // some enemy is close enough for the lookahead to matter
int thS = 1;

// Cells an enemy at src can put its head on this turn (freeR free steps, up to
// maxR with paid ones). `cnt`/`margin` (optional) also count attackers and
// record how far beyond the reach a cell lies, up to QUEEN_MARGIN more steps.
void reach_bfs(int src, int freeR, int maxR, std::vector<int>& out, std::vector<int>* cnt = nullptr, std::vector<int>* margin = nullptr)
{
    thS++;
    thQ.clear();
    thQ.push_back(src);
    thStamp[src] = thS;
    thDist[src] = 0;
    int const lim = maxR + (margin ? QUEEN_MARGIN : 0);
    for (std::size_t i = 0; i < thQ.size(); i++)
    {
        int const c = thQ[i], dc = thDist[c];
        if (dc >= lim)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int n = cross(c, k);
            if (n == -2)
                n = nb(c, k); // unexplored edge: assume the enemy can pass
            if (n < 0 || thStamp[n] == thS)
                continue;
            thStamp[n] = thS;
            thDist[n] = dc + 1;
            if (dc + 1 <= maxR)
            {
                out[n] = std::min(out[n], std::max(0, dc + 1 - freeR));
                if (cnt)
                    (*cnt)[n]++;
            }
            else if (margin)
                (*margin)[n] = std::min((*margin)[n], dc + 1 - maxR);
            if (vis[n] && occId[n] >= 0)
                continue; // a body: the enemy cannot go through it
            thQ.push_back(n);
        }
    }
}

void compute_queen_threat()
{
    std::fill(qThreat.begin(), qThreat.end(), INF);
    std::fill(qThreatVis.begin(), qThreatVis.end(), INF);
    std::fill(qThreatCnt.begin(), qThreatCnt.end(), 0);
    std::fill(qMargin.begin(), qMargin.end(), INF);
    std::vector<int>* cnt = QUEEN_HARD_THREAT ? &qThreatCnt : nullptr;
    std::vector<int>* mar = QUEEN_HARD_THREAT ? &qMargin : nullptr;
    // P3a: her last segment leaves its cell when she steps, so an enemy can
    // come through it (a U-turn is no shield).
    int tailCell = -1, tailOcc = -1;
    if (THREAT_TAIL_FREE && amQueen && static_cast<int>(body.size()) >= L && L >= 2)
    {
        tailCell = body[L - 1];
        tailOcc = occId[tailCell];
        if (tailOcc == me)
            occId[tailCell] = -1;
        else
            tailCell = -1;
    }
    std::unordered_set<int> seenIds;
    if (QUEEN_LOOK2)
        std::fill(qThreat2.begin(), qThreat2.end(), INF);
    look2Active = false;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        seenIds.insert(h.id);
        int const len = std::max(h.len, h.estLen);
        int const f = std::max(1, (len + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
        reach_bfs(h.cell, f, f, qThreatVis);
        reach_bfs(h.cell, f, std::max(f, m), qThreat, cnt, mar);
        if (QUEEN_LOOK2)
        {
            int const m2 = std::min(PAID_REACH_CAP, m + f); // two turns: free steps twice, the paid budget once
            reach_bfs(h.cell, 2 * f, m2, qThreat2);
            if (!body.empty() && manhattan(h.cell, body[0]) <= m2 + LOOK2_QUEEN_REACH + 1)
                look2Active = true;
        }
    }
    // Enemies the fleet reported in the last two rounds: they may have moved
    // a free step or two since.
    for (auto const& e : enemyReps)
    {
        int const age = R - e.round;
        if (age < 1 || age > REP_MAX_AGE || seenIds.count(e.id))
            continue;
        int const el = std::max(e.len, e.estLen);
        int const f = std::max(1, (el + 3) / 4) * (age + 1);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, el - 2) : 0));
        reach_bfs(e.cell, f, std::max(f, m), qThreat, cnt, mar);
        if (QUEEN_LOOK2)
        {
            int const f1 = std::max(1, (el + 3) / 4);
            int const m2 = std::min(PAID_REACH_CAP, m + f1);
            reach_bfs(e.cell, f + f1, m2, qThreat2);
            if (!body.empty() && manhattan(e.cell, body[0]) <= m2 + LOOK2_QUEEN_REACH + 1)
                look2Active = true;
        }
    }
    if (tailCell >= 0)
        occId[tailCell] = tailOcc;
}

// Path distances from the queen's head out to `maxR` steps (known water;
// unexplored edges count as open). Used by teammates to give her room.
std::vector<int> qDist;
void queen_dist_bfs(int src, int maxR)
{
    std::fill(qDist.begin(), qDist.end(), INF);
    thQ.clear();
    thQ.push_back(src);
    qDist[src] = 0;
    for (std::size_t i = 0; i < thQ.size(); i++)
    {
        int const c = thQ[i];
        if (qDist[c] >= maxR)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int n = cross(c, k);
            if (n == -2)
                n = nb(c, k);
            if (n < 0 || qDist[n] != INF)
                continue;
            qDist[n] = qDist[c] + 1;
            thQ.push_back(n);
        }
    }
}

// Open sides of a cell (kelp and unexplored edges excluded): <= 2 is a corridor.
int open_sides(int c)
{
    int k = 0;
    for (int d = 0; d < 4; d++)
        k += cross(c, d) >= 0;
    return k;
}

// A cell inside a 1-wide dead end (a pocket): following the corridor cells
// from it reaches a dead end before any open water.
bool in_pocket(int c)
{
    if (open_sides(c) >= 3)
        return false;
    std::vector<int> q{c};
    std::unordered_set<int> seen{c};
    for (std::size_t i = 0; i < q.size() && q.size() <= 12; i++)
    {
        int const x = q[i];
        int const os = open_sides(x);
        if (os <= 1)
            return true;
        if (os >= 3)
            continue; // opens out this way
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(x, d);
            if (n >= 0 && seen.insert(n).second)
                q.push_back(n);
        }
    }
    return false;
}

// Stepping from `from` into `to`: does the 1-wide corridor ahead end in a
// dead end before it opens out? (Unexplored edges count as closed.)
bool pocket_ahead(int from, int to)
{
    int prev = from, c = to;
    for (int k = 0; k < 24; k++)
    {
        int const os = open_sides(c);
        if (os <= 1)
            return true;
        if (os >= 3)
            return false;
        int next = -1;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n >= 0 && n != prev)
                next = n;
        }
        if (next < 0)
            return true;
        prev = c;
        c = next;
    }
    return false;
}

// The queen's resting spot: the most open known water inside her leash
// (cells all round with four open sides), never in a pocket.
int choose_rest()
{
    if (ourQueenSpawn < 0)
        return -1;
    int best = -1;
    double bestScore = -1e18;
    int const hx = ourQueenSpawn % W, hy = ourQueenSpawn / W;
    for (int dy = -QUEEN_LEASH; dy <= QUEEN_LEASH; dy++)
        for (int dx = -QUEEN_LEASH + std::abs(dy); dx <= QUEEN_LEASH - std::abs(dy); dx++)
        {
            int const c = cell_of(((hx + dx) % W + W) % W, ((hy + dy) % H + H) % H);
            if (seenRound[c] < 0 || distHead[c] >= INF || in_pocket(c))
                continue;
            double sc = 0;
            for (int ey = -2; ey <= 2; ey++)
                for (int ex = -2 + std::abs(ey); ex <= 2 - std::abs(ey); ex++)
                {
                    int const n = cell_of(((c % W + ex) % W + W) % W, ((c / W + ey) % H + H) % H);
                    sc += seenRound[n] >= 0 ? open_sides(n) : 1;
                }
            sc -= 0.5 * (std::abs(dx) + std::abs(dy));
            for (auto const& e : enemyReps)
                if (R - e.round <= 20 && manhattan(e.cell, c) <= 6)
                    sc -= 10;
            if (sc > bestScore)
            {
                bestScore = sc;
                best = c;
            }
        }
    return best;
}

// P3c: after a move to `dest` with body `nb` (newLen long), how many cells
// can she put her head on next turn (free + paid steps, her own body in the
// way except its last segment) that no enemy can reach within two turns and
// that are not dead ends? Capped at 8.
int look2_safe_cells(int dest, std::vector<int> const& nb, int newLen)
{
    l2S++;
    int const reachQ = std::min(LOOK2_QUEEN_REACH, (newLen + 3) / 4 + (LOOK2_PAID ? std::max(0, newLen - 2) : 0));
    for (int i = 0; i + 1 < newLen && i < static_cast<int>(nb.size()); i++)
        l2Stamp[nb[i]] = l2S; // her body after the move (the last segment moves on)
    l2Q.clear();
    l2Q.push_back(dest);
    l2Stamp[dest] = l2S;
    l2Dist[dest] = 0;
    int safe = 0;
    for (std::size_t i = 0; i < l2Q.size() && safe < 8; i++)
    {
        int const c = l2Q[i];
        if (l2Dist[c] >= reachQ)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(c, k);
            if (n < 0 || l2Stamp[n] == l2S || (vis[n] && occId[n] >= 0 && occId[n] != me))
                continue;
            l2Stamp[n] = l2S;
            l2Dist[n] = l2Dist[c] + 1;
            l2Q.push_back(n);
            if (qThreat2[n] >= INF && seenRound[n] >= 0 && open_sides(n) >= 2)
                safe++;
        }
    }
    return safe;
}

double queen_threat_pen(int n)
{
    if (!QUEEN_HARD_THREAT)
    {
        if (qThreat[n] == 0)
            return 3e6;
        if (qThreat[n] < INF)
            return PAID_THREAT_PEN / static_cast<double>(qThreat[n]);
        return 0;
    }
    // Every enemy moves after the queen, and a head-on kills both whatever the
    // lengths, so paying segments to reach her is free for the attacker:
    // anything within free + paid reach is as good as a kill.
    double p = 0;
    if (qThreat[n] == 0)
        p = 3e6;
    else if (qThreat[n] < INF)
        p = HARD_THREAT_PEN - 20000.0 * std::min(qThreat[n], 10);
    if (qThreatCnt[n] > 1)
        p += THREAT_EXTRA_ATTACKER * (qThreatCnt[n] - 1);
    if (qThreat[n] == INF && qMargin[n] <= QUEEN_MARGIN)
        p += QUEEN_MARGIN_PEN * (QUEEN_MARGIN + 1 - qMargin[n]);
    return p;
}

double end_threat(int n)
{
    double t = 0;
    for (auto const& hh : heads)
    {
        if (hh.mine)
        {
            if (manhattan(hh.cell, n) <= 1)
                t += 4000;
            continue;
        }
        if (amQueen && QUEEN_THREAT_MAP)
            continue; // added once below from the threat map
        int const reach = isLeader ? enemy_reach(hh) : 1;
        if (manhattan(hh.cell, n) <= reach)
            t += amQueen ? 3e6 : isLeader ? 300000 : THREAT_PEN;
    }
    if (amQueen && QUEEN_THREAT_MAP)
        t += queen_threat_pen(n);
    if (isLeader)
        for (int tc : enemyTails)
            if (manhattan(tc, n) <= 1)
                t += amQueen ? 400000 : 100000;
    return t;
}

int queen_exits(int qh, std::vector<int> const& newBody);

// Depth-first search over step sequences through cells in view. Other dragons
// do not move during our turn, so what we see is exact; our own body shifts
// step by step. Steps past freeN cost a segment each.
void sprint_dfs(std::vector<int> const& b, int len, int step, int maxSteps, int freeN, int eaten,
                std::vector<int>& dirs, std::vector<int>& eatenCells, bool useGoal, int goalStart, SprintPlan& best)
{
    int const h = b[0];
    for (int d = 0; d < 4; d++)
    {
        int const n = cross(h, d);
        if (n < 0 || !vis[n])
            continue;
        if (occId[n] >= 0 && occId[n] != me)
        {
            // Head-on with the enemy queen: worth any non-queen dragon.
            bool const canPay = step + 1 <= freeN || len >= 3;
            if (canPay && occHead[n] && occId[n] == enemyQueenId && !occMine[n] && !amQueen && !enemyQueenDead && !best.kill)
            {
                dirs.push_back(d);
                best.dirs = dirs;
                best.kill = true;
                best.value = 1e12;
                dirs.pop_back();
            }
            continue;
        }
        set_body(b, len);
        if (!passable(n, 1))
            continue;
        bool const eat = pearl_at(n) && std::find(eatenCells.begin(), eatenCells.end(), n) == eatenCells.end();
        std::vector<int> nb2;
        nb2.reserve(b.size() + 1);
        nb2.push_back(n);
        nb2.insert(nb2.end(), b.begin(), b.end());
        int nl = len + (eat ? 1 : 0);
        if (!eat && static_cast<int>(b.size()) >= len)
            nb2.pop_back();
        if (step + 1 > freeN) // a paid step also removes one more tail segment
        {
            if (static_cast<int>(nb2.size()) >= nl)
                nb2.pop_back();
            nl--;
            if (nl < 2)
                continue;
        }
        if (eat)
            eatenCells.push_back(n);
        dirs.push_back(d);
        int const eatenNow = eaten + (eat ? 1 : 0);
        if (step + 1 >= 2 && !best.kill)
        {
            int const tailCell = static_cast<int>(nb2.size()) >= nl ? nb2.back() : -1;
            set_body(nb2, nl);
            int const cap = std::min(N, 3 * nl + 24);
            FloodRes const f = flood(n, tailCell, cap);
            int const area = f.area + (f.frontier && !amQueen ? nl : 0);
            if (f.tail || area >= 2 * nl + 4)
            {
                double v = eatenNow * 3000.0 + std::min(area, cap) * 15.0 - (L + eatenNow - nl) * 4000.0 - end_threat(n);
                if (QUEEN_EXIT_GUARD && !amQueen)
                    for (auto const& qh : heads)
                        if (qh.mine && qh.id == queenId && manhattan(qh.cell, n) <= 2)
                        {
                            std::vector<int> nowBody(body.begin(), body.begin() + std::min<int>(body.size(), L));
                            std::vector<int> after(nb2.begin(), nb2.begin() + std::min<int>(nb2.size(), nl));
                            int const before = queen_exits(qh.cell, nowBody), now = queen_exits(qh.cell, after);
                            if (now < before && now < 2)
                                v -= QUEEN_EXIT_PEN; // a sprint must not end in the queen's last way out
                        }
                if (useGoal && distGoal[n] < INF)
                    v += 150.0 * (goalStart - distGoal[n]);
                if (v > best.value)
                {
                    best.value = v;
                    best.dirs = dirs;
                }
            }
        }
        if (step + 1 < maxSteps && !best.kill)
            sprint_dfs(nb2, nl, step + 1, maxSteps, freeN, eatenNow, dirs, eatenCells, useGoal, goalStart, best);
        dirs.pop_back();
        if (eat)
            eatenCells.pop_back();
    }
}

void act_moves(Controller& ct, std::vector<int> const& dirs, char const* why)
{
    std::vector<Direction> ds;
    for (int d : dirs)
        ds.push_back(dir_of(d));
    ct.make_moves(ds);
    note = why;
    prevBody = body;
    lastDirs = dirs;
}

// Best guess of the enemy queen's head: in view, recently reported, or the
// mirror image of our queen's start (one guess per symmetry still possible).
// Their feeders die next to their queen: three or more recent death drops
// close together (and not ours) mark where she probably is.
int feeder_queen_guess()
{
    if (!FEEDER_TRAIL || R < 100)
        return -1;
    std::vector<int> ev;
    for (auto const& e : dropEv)
    {
        if (R - e.round > 20 || e.nearOurQueen)
            continue;
        bool ours = false; // one of our dragons died there (a trade or an accident)
        for (auto const& dg : dangers)
            if (std::abs(dg.round - e.round) <= 3 && manhattan(dg.cell, e.cell) <= 3)
                ours = true;
        if (!ours)
            ev.push_back(e.cell);
    }
    int best = -1, bestCnt = 2;
    for (int a : ev)
    {
        int cnt = 0;
        for (int b : ev)
            if (b != a && manhattan(a, b) <= 5)
                cnt++;
        if (cnt >= bestCnt)
        {
            bestCnt = cnt;
            best = a;
        }
    }
    return best;
}

int enemy_queen_target()
{
    for (auto const& h : heads)
        if (!h.mine && h.id == enemyQueenId)
            return h.cell;
    int best = -1, bestRound = -1;
    for (auto const& e : enemyReps)
        if (e.id == enemyQueenId && R - e.round <= 30 && e.round > bestRound)
        {
            best = e.cell;
            bestRound = e.round;
        }
    if (best >= 0)
        return best;
    int const trail = feeder_queen_guess();
    if (trail >= 0)
        return trail;
    if (ourQueenSpawn < 0)
        return -1;
    std::vector<int> ks;
    for (int k = 0; k < 3; k++)
        if (symMask & (1 << k))
            ks.push_back(k);
    if (ks.empty())
        return -1;
    return mirror_cell(ks[me % ks.size()], ourQueenSpawn);
}

// Screen post: a cell SCREEN_MIN..SCREEN_MAX steps from the queen along her
// row (even ids) or column (odd ids), on the side nearer the enemy queen.
// Sonar runs along rows and columns, so from there a ping reaches her.
int screen_post(int qCell)
{
    // Face the threat: the nearest enemy reported near her lately, else the
    // enemy queen (or their side of the map).
    int eq = -1, ed = INF;
    if (SCREENS_MORE)
        for (auto const& e : enemyReps)
            if (R - e.round <= 10 && manhattan(e.cell, qCell) <= 15 && manhattan(e.cell, qCell) < ed)
            {
                ed = manhattan(e.cell, qCell);
                eq = e.cell;
            }
    if (eq < 0)
        eq = enemy_queen_target();
    int best = -1, bestScore = INF;
    bool const row = (me & 1) == 0;
    for (int off = -SCREEN_MAX; off <= SCREEN_MAX; off++)
    {
        if (std::abs(off) < SCREEN_MIN)
            continue;
        int const x = qCell % W, y = qCell / W;
        int const nx = row ? ((x + off) % W + W) % W : x, ny = row ? y : ((y + off) % H + H) % H;
        int const c = cell_of(nx, ny);
        if (distHead[c] >= INF || manhattan(c, qCell) < SCREEN_MIN)
            continue;
        int const sc = (eq >= 0 ? 4 * manhattan(c, eq) : 0) + distHead[c];
        if (sc < bestScore)
        {
            bestScore = sc;
            best = c;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Bunker: once the enemy queen is dead, ours only has to stay alive. A queen
// of length 2-3 can circle a 2x2 box for ever (verified in the engine; at
// length 4 her own tail blocks the loop). Boxes with few open sides are safer.
// ---------------------------------------------------------------------------
int bunkerBox = -1;

// The box with top-left tl, in loop order (tl, east, south-east, south), when
// all four links inside it are known open water.
bool box_cells(int tl, std::array<int, 4>& out)
{
    auto const open = [](int c, int d) { return edgeType[edge_code(c, d)] == E_OPEN; };
    if (!open(tl, 1) || !open(tl, 2))
        return false;
    int const e = nb(tl, 1), s = nb(tl, 2), se = nb(e, 2);
    if (!open(e, 2) || !open(s, 1) || se == tl || e == s)
        return false;
    out = {tl, e, se, s};
    return true;
}

// Ways into the box from outside (unexplored edges count as open).
int box_exposure(std::array<int, 4> const& bx)
{
    int k = 0;
    for (int c : bx)
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -1 || std::find(bx.begin(), bx.end(), n) != bx.end())
                continue;
            k++;
        }
    return k;
}

// Usable now: no other dragon in it, not in enemy reach, and no pearl in it or
// due there (eating fills the box: a queen of length 4 in a 2x2 has no move).
bool box_usable(std::array<int, 4> const& bx)
{
    for (int c : bx)
    {
        if (vis[c] && occId[c] >= 0 && occId[c] != me)
            return false;
        if (qThreat[c] < INF)
            return false;
        if (pearlState[c] == 1 || obsNext[c] >= 0)
            return false;
        for (int k = 0; k < 4; k++)
        {
            std::uint8_t const t = edgeType[edge_code(c, k)];
            if (t != E_OPEN && t != E_KELP)
                return false; // no portals or unexplored edges around the bunker
        }
    }
    return true;
}

// A queen can never be trapped while she can reach a free loop longer than
// she is: she can circle it for ever and turn round there (a 2x2 for length
// <= 3, verified in the engine; a k x k ring holds 4(k-1) cells). Unexplored
// edges do not count here: pockets with unseen walls are how queens die.
bool ring_free(int tl, int k)
{
    std::vector<int> ring;
    int c = tl;
    int const dirs[4] = {1, 2, 3, 0}; // E along the top, S down the right, W along the bottom, N up the left
    for (int side = 0; side < 4; side++)
        for (int i = 0; i < k - 1; i++)
        {
            ring.push_back(c);
            if (edgeType[edge_code(c, dirs[side])] != E_OPEN)
                return false;
            c = nb(c, dirs[side]);
        }
    if (c != tl)
        return false;
    for (int x : ring)
        if ((vis[x] && occId[x] >= 0 && occId[x] != me) || wallStamp[x] == wstamp)
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Static topology of the known map (ported from v3.x step 7b: v2.14.11)
// ---------------------------------------------------------------------------
// A dragon can never turn round where there is no cycle: its own tail blocks
// the cell it came from. So known water splits into LOOP REGIONS (2-edge-
// connected components: every edge in one lies on a cycle) joined by BRIDGES.
// Crossing a bridge leads into a BRANCH. A branch with no loop longer than the
// dragon and no unexplored edge is a trap it can only leave by splitting.
// Edges are the known crossings (open water and portals with a known far end);
// an unexplored edge or an unlocated portal marks its cell as FRONTIER (the
// map may go on there). Rebuilt lazily whenever an edge is learned.
struct Branch
{
    int cells = 0;
    int maxCap = 0;    // largest loop region in the branch (0: no cycle at all)
    int spawns = 0;    // known spawn tiles
    bool frontier = false;
};
Branch merge(Branch a, Branch const& b)
{
    a.cells += b.cells;
    a.maxCap = std::max(a.maxCap, b.maxCap);
    a.spawns += b.spawns;
    a.frontier = a.frontier || b.frontier;
    return a;
}
struct Topology
{
    int version = -1;
    std::vector<int> comp;            // cell -> loop region
    std::vector<int> cap;             // region -> cells (its longest cycle is at most this; a region of 1 has none)
    std::vector<char> bridge;         // edge key -> crossing it splits the known map
    std::vector<int> parent;          // region -> parent region in the bridge forest (-1: a root)
    std::vector<Branch> own, down, up; // region itself / its subtree / everything beyond its parent bridge
} topo;

// One physical edge seen from either side: a portal's two edges count as one.
int edge_key(int c, int d)
{
    int const code = edge_code(c, d);
    return edgeType[code] == E_PORTAL && edgePartner[code] >= 0 ? std::min(code, edgePartner[code]) : code;
}

void topo_build()
{
    topo.version = edgeVersion;
    static std::vector<int> start, to, key, tin, low, order;
    start.assign(N + 1, 0);
    to.clear();
    key.clear();
    static std::vector<char> front;
    front.assign(N, 0);
    for (int c = 0; c < N; c++)
    {
        start[c] = static_cast<int>(to.size());
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n == -2 || n == -3)
                front[c] = 1;
            else if (n >= 0 && n != c)
            {
                to.push_back(n);
                key.push_back(edge_key(c, d));
            }
        }
    }
    start[N] = static_cast<int>(to.size());

    // Bridges: iterative Tarjan (no recursion - the wasm stack is small). The
    // edge we arrived by is skipped by its key, so parallel edges are cycles.
    topo.bridge.assign(2 * N, 0);
    tin.assign(N, -1);
    low.assign(N, 0);
    struct Frame
    {
        int v, parentKey, next;
    };
    static std::vector<Frame> st;
    st.clear();
    int timer = 0;
    for (int s = 0; s < N; s++)
    {
        if (tin[s] >= 0)
            continue;
        tin[s] = low[s] = timer++;
        st.push_back({s, -1, start[s]});
        while (!st.empty())
        {
            int const v = st.back().v;
            if (st.back().next < start[v + 1])
            {
                int const i = st.back().next++;
                int const u = to[i];
                if (key[i] == st.back().parentKey)
                    continue;
                if (tin[u] >= 0)
                    low[v] = std::min(low[v], tin[u]);
                else
                {
                    tin[u] = low[u] = timer++;
                    st.push_back({u, key[i], start[u]});
                }
            }
            else
            {
                int const pk = st.back().parentKey;
                st.pop_back();
                if (!st.empty())
                {
                    int const p = st.back().v;
                    low[p] = std::min(low[p], low[v]);
                    if (low[v] > tin[p])
                        topo.bridge[pk] = 1;
                }
            }
        }
    }

    // Loop regions: connected by non-bridge edges.
    topo.comp.assign(N, -1);
    topo.cap.clear();
    topo.own.clear();
    static std::vector<int> q;
    for (int s = 0; s < N; s++)
    {
        if (topo.comp[s] >= 0)
            continue;
        int const id = static_cast<int>(topo.cap.size());
        Branch b;
        q.assign(1, s);
        topo.comp[s] = id;
        for (std::size_t k = 0; k < q.size(); k++)
        {
            int const c = q[k];
            b.cells++;
            b.spawns += nextSpawn[c] >= 0 || gapHi[c] > 0;
            b.frontier = b.frontier || front[c];
            for (int i = start[c]; i < start[c + 1]; i++)
                if (!topo.bridge[key[i]] && topo.comp[to[i]] < 0)
                {
                    topo.comp[to[i]] = id;
                    q.push_back(to[i]);
                }
        }
        b.maxCap = b.cells >= 2 ? b.cells : 0;
        topo.cap.push_back(b.cells);
        topo.own.push_back(b);
    }

    // Bridge forest over the regions (flat arrays, reused between builds: on a half-explored big map most
    // unexplored cells are regions of their own); branch summaries in both directions.
    int const R0 = static_cast<int>(topo.cap.size());
    static std::vector<int> fStart, fAdj, chBuf;
    fStart.assign(R0 + 1, 0);
    for (int c = 0; c < N; c++)
        for (int i = start[c]; i < start[c + 1]; i++)
            if (topo.bridge[key[i]])
                fStart[topo.comp[c] + 1]++;
    for (int r = 0; r < R0; r++)
        fStart[r + 1] += fStart[r];
    fAdj.assign(fStart[R0], 0);
    {
        static std::vector<int> fill;
        fill.assign(fStart.begin(), fStart.end() - 1);
        for (int c = 0; c < N; c++)
            for (int i = start[c]; i < start[c + 1]; i++)
                if (topo.bridge[key[i]])
                    fAdj[fill[topo.comp[c]]++] = topo.comp[to[i]];
    }
    topo.parent.assign(R0, -2);
    topo.down.assign(R0, Branch{});
    topo.up.assign(R0, Branch{});
    order.clear();
    for (int r = 0; r < R0; r++)
    {
        if (topo.parent[r] != -2)
            continue;
        topo.parent[r] = -1;
        std::size_t const first = order.size();
        order.push_back(r);
        for (std::size_t k = first; k < order.size(); k++)
            for (int i = fStart[order[k]]; i < fStart[order[k] + 1]; i++)
                if (topo.parent[fAdj[i]] == -2)
                {
                    topo.parent[fAdj[i]] = order[k];
                    order.push_back(fAdj[i]);
                }
    }
    for (int k = static_cast<int>(order.size()) - 1; k >= 0; k--) // children before parents
    {
        int const x = order[k];
        topo.down[x] = merge(topo.down[x], topo.own[x]);
        if (topo.parent[x] >= 0)
            topo.down[topo.parent[x]] = merge(topo.down[topo.parent[x]], topo.down[x]);
    }
    static std::vector<Branch> pre;
    for (int p : order) // parents before children
    {
        if (fStart[p] == fStart[p + 1])
            continue; // a leaf (most regions): no children to fill in
        chBuf.clear();
        for (int i = fStart[p]; i < fStart[p + 1]; i++)
            if (topo.parent[fAdj[i]] == p && std::find(chBuf.begin(), chBuf.end(), fAdj[i]) == chBuf.end())
                chBuf.push_back(fAdj[i]);
        Branch const base = merge(topo.up[p], topo.own[p]);
        pre.assign(chBuf.size() + 1, Branch{});
        for (std::size_t i = 0; i < chBuf.size(); i++)
            pre[i + 1] = merge(pre[i], topo.down[chBuf[i]]);
        Branch suf;
        for (int i = static_cast<int>(chBuf.size()) - 1; i >= 0; i--)
        {
            topo.up[chBuf[i]] = merge(merge(base, pre[i]), suf);
            suf = merge(suf, topo.down[chBuf[i]]);
        }
    }
}

// Rebuilt at most every TOPO_EVERY rounds (CPU). A stale topology can only MISS a new trap, never invent one: a
// branch judged a trap is fully explored, so no new edge can touch it or reconnect it.
int topoBuiltR = -1000;
void topo_refresh()
{
    if (static_cast<int>(topo.comp.size()) != N || (topo.version != edgeVersion && R - topoBuiltR >= TOPO_EVERY))
    {
        topo_build();
        topoBuiltR = R;
    }
}

// The branch a step from `from` in direction d leads into; false when the step
// does not cross a bridge (it stays inside a loop region).
bool topo_beyond(int from, int d, Branch& out)
{
    topo_refresh();
    int const n = cross(from, d);
    if (n < 0 || !topo.bridge[edge_key(from, d)])
        return false;
    int const a = topo.comp[from], b = topo.comp[n];
    if (topo.parent[b] == a)
        out = topo.down[b];
    else if (topo.parent[a] == b)
        out = topo.up[a];
    else
        return false;
    return true;
}

// A known trap for a dragon of length len: the step crosses a bridge into a
// fully explored branch that has no loop longer than the dragon.
bool topo_trap(int from, int d, int len)
{
    Branch b;
    return topo_beyond(from, d, b) && !b.frontier && b.maxCap <= len;
}

// Unexplored edge = open water (planning through the enemy side, which is mostly unexplored).
int step_open(int c, int d)
{
    int const n = cross(c, d);
    return n == -2 ? nb(c, d) : n;
}

// CHOKE_GUARDS: the first bridge (topology) on the queen's shortest way toward the enemy side, within
// CHOKE_RANGE steps of her: the cell on her side of it. One guard there sees (and can block) everything that comes
// through. -1 when her way out has no bridge that close.
int choke_post_search(int qCell)
{
    int const es = enemy_spawn_guess();
    if (qCell < 0 || es < 0)
        return -1;
    topo_refresh();
    static std::vector<int> par, q;
    par.assign(N, -2);
    q.assign(1, qCell);
    par[qCell] = -1;
    int hit = -1;
    for (std::size_t i = 0; i < q.size() && hit < 0; i++)
        for (int d = 0; d < 4; d++)
        {
            int const n = step_open(q[i], d); // only the bridges need known edges
            if (n < 0 || par[n] != -2)
                continue;
            par[n] = q[i];
            if (n == es || manhattan(n, es) <= 2)
            {
                hit = n;
                break;
            }
            q.push_back(n);
        }
    if (hit < 0)
        return -1;
    std::vector<int> path; // queen -> enemy side
    for (int c = hit; c != -1; c = par[c])
        path.push_back(c);
    std::reverse(path.begin(), path.end());
    for (int i = 0; i + 1 < static_cast<int>(path.size()) && i < CHOKE_RANGE; i++)
        for (int d = 0; d < 4; d++)
            if (cross(path[i], d) == path[i + 1] && cross(path[i], d) >= 0 && topo.bridge[edge_key(path[i], d)])
                return i >= 2 ? path[i] : -1; // not right next to her: a guard there would box her in
    return -1;
}
// CHOKE_GUARDS: a guard leaves for the post only while another (non-screen) guard with a lower id is by her - in view
// or reported within the last 3 rounds, within GUARD_RING. The lowest-id guard near her never leaves.
// Guard slots follow from the id alone (the same hash as the role split).
int role_slot(int id)
{
    std::uint32_t const hh = static_cast<std::uint32_t>(id) * 2246822519u + 3266489917u;
    return static_cast<int>((hh >> 13) % 10);
}
bool guard_stays_by_her(int qCell)
{
    for (auto const& [id, st] : fleet)
    {
        if (id >= me || id == queenId)
            continue;
        int const slot = role_slot(id);
        bool const guardSlot = slot >= HUNTER_SHARE && slot < HUNTER_SHARE + GUARD_SHARE;
        bool const screenSlot = SCREENS && (slot == HUNTER_SHARE || (SCREENS_MORE && units < 20 && slot == HUNTER_SHARE + 1 && (id & 1)));
        if (!guardSlot || screenSlot || st.leader)
            continue;
        int cell = R - st.round <= 3 ? st.cell : -1;
        for (auto const& h : heads)
            if (h.mine && h.id == id)
                cell = h.cell;
        if (cell >= 0 && manhattan(cell, qCell) <= GUARD_RING)
            return true;
    }
    return false;
}
int choke_post(int qCell)
{
    static int cacheR = -1, cacheQ = -1, cacheV = -1; // once per turn
    if (cacheR != R || cacheQ != qCell)
    {
        cacheR = R;
        cacheQ = qCell;
        cacheV = choke_post_search(qCell);
    }
    return cacheV;
}

// Long queens (no k x k ring test): a loop region longer than her within
// QUEEN_LOOP_DEPTH steps of her head through known open water.
bool topo_loop_near(std::vector<int> const& b, int len)
{
    topo_refresh();
    set_body(b, len);
    std::unordered_map<int, int> dist{{b[0], 0}};
    std::vector<int> q{b[0]};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (topo.cap[topo.comp[c]] >= std::max(2, len + 1) || topo.own[topo.comp[c]].frontier)
            return true;
        if (dc >= QUEEN_LOOP_DEPTH)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (edgeType[edge_code(c, d)] != E_OPEN)
                continue;
            int const n = nb(c, d);
            if (dist.count(n) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}

bool loop_reachable(std::vector<int> const& b, int len)
{
    int k = 2;
    while (4 * (k - 1) <= len)
        k++;
    if (k > 5)
        return !TOPO_LOOP || topo_loop_near(b, len); // long queens: no k x k ring test; a loop region longer than her
    set_body(b, len);
    std::unordered_map<int, int> dist{{b[0], 0}};
    std::vector<int> q{b[0]};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (ring_free(c, k))
            return true;
        if (dc >= QUEEN_LOOP_DEPTH)
            continue;
        for (int d = 0; d < 4; d++)
        {
            if (edgeType[edge_code(c, d)] != E_OPEN)
                continue;
            int const n = nb(c, d);
            if (dist.count(n) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}

// Optional queen split: does she still have a loop with the child's body (the
// tail part) standing where it is?
bool split_keeps_loop(int child)
{
    if (!QUEEN_LOOP_SAFE || static_cast<int>(body.size()) < L)
        return true;
    std::vector<int> front(body.begin(), body.begin() + (L - child));
    wstamp++;
    for (int i = L - child; i < L; i++)
        wallStamp[body[i]] = wstamp;
    bool const ok = loop_reachable(front, L - child);
    wstamp++;
    set_body(body, L);
    return ok;
}

int choose_bunker()
{
    int best = -1;
    double bestS = 1e18;
    std::array<int, 4> bx{};
    for (int c = 0; c < N; c++)
    {
        if (distHead[c] > BUNKER_SEARCH && c != body[0])
            continue;
        if (!box_cells(c, bx) || !box_usable(bx))
            continue;
        int d = INF;
        for (int x : bx)
            d = std::min(d, x == body[0] ? 0 : distHead[x]);
        if (d >= INF)
            continue;
        double sc = 8.0 * box_exposure(bx) + d;
        for (int x : bx)
            if (qMargin[x] <= QUEEN_MARGIN)
                sc += 6.0 * (QUEEN_MARGIN + 1 - qMargin[x]);
        for (auto const& e : enemyReps)
            if (R - e.round <= 10)
                sc += 40.0 / (1 + manhattan(e.cell, c));
        if (sc < bestS)
        {
            bestS = sc;
            best = c;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Packs: hunters (and, once our queen is dead, every small dragon) share out
// prey the same way pearls are shared: every dragon solves the same greedy
// matching from what the fleet knows, so packs form without negotiation.
// ---------------------------------------------------------------------------
int hunter_slot(int id)
{
    std::uint32_t const hh = static_cast<std::uint32_t>(id) * 2246822519u + 3266489917u;
    return static_cast<int>((hh >> 13) % 10);
}

bool hunts_now()
{
    return HUNTERS && !(enemyQueenDead && !ourQueenDead) && (R >= HUNT_START || !expansionPhase) && R >= OPEN_FARM_ROUND;
}

// Would teammate `id` (length len) be in a pack right now?
bool pack_member(int id, int len, bool leader)
{
    if (id == queenId || leader)
        return false;
    if (ENDGAME_PACKS && ourQueenDead && len <= 6)
        return true;
    return hunts_now() && hunter_slot(id) < HUNTER_SHARE && len >= HUNTER_LEN;
}

struct Prey
{
    int cell, id, len;
    bool queen;
};
std::vector<Prey> preyList;
int myPrey = -1, myPackSize = 0, myPackRank = 0;

void assign_prey()
{
    preyList.clear();
    myPrey = -1;
    myPackSize = 0;
    myPackRank = 0;
    // Our queen is dead: only the longest dragon can still win it. If theirs
    // has not been seen for 100 rounds she is most likely dead too (theirs
    // die early), so their longest dragons come first.
    bool const giantHunt = GIANT_HUNT && ourQueenDead && R >= GIANT_HUNT_ROUND && (enemyQueenDead || R - lastEqSeenR >= 100);
    int qt = enemyQueenDead || giantHunt ? -1 : enemy_queen_target();
    // Big map, her position not seen lately: she hides at home among her
    // swarm, and the long walk there only feeds it (ladder: never reached).
    if (HUNT_FAR_SMALL && qt >= 0 && N > SMALL_MAP_CELLS && !ourQueenDead && R - lastEqSeenR > HUNT_FRESH)
        qt = -1;
    if (qt >= 0)
        preyList.push_back({qt, enemyQueenId, 0, true});
    // Long enemy dragons: when the queens no longer decide the game, or while
    // nobody knows where the enemy queen is.
    if (ourQueenDead || enemyQueenDead || qt < 0)
        for (auto const& e : enemyReps)
            if (e.id != enemyQueenId && R - e.round <= 6 && e.len >= (giantHunt ? GIANT_PREY_MIN : PREY_MIN_LEN))
                preyList.push_back({e.cell, e.id, e.len, false});
    if (preyList.empty())
        return;

    struct Agent
    {
        int id, cell;
    };
    std::vector<Agent> agents{{me, body[0]}};
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 4 && pack_member(id, st.len, st.leader))
            agents.push_back({id, st.cell});
    struct Pair
    {
        double cost;
        int a, p;
    };
    std::vector<Pair> pairs;
    for (int pi = 0; pi < static_cast<int>(preyList.size()); pi++)
        for (int ai = 0; ai < static_cast<int>(agents.size()); ai++)
        {
            int const d = agents[ai].id == me ? dist_to(distHead, preyList[pi].cell) : manhattan(agents[ai].cell, preyList[pi].cell);
            if (d >= INF)
                continue;
            double const c = d - (preyList[pi].queen ? 8.0 : (giantHunt ? 1.0 : 0.25) * preyList[pi].len) + agents[ai].id * 1e-6;
            pairs.push_back({c, ai, pi});
        }
    std::sort(pairs.begin(), pairs.end(), [](Pair const& x, Pair const& y) { return x.cost < y.cost; });
    std::vector<int> taken(preyList.size(), 0);
    std::vector<char> used(agents.size(), 0);
    std::vector<int> assignedTo(agents.size(), -1);
    for (auto const& pr : pairs)
    {
        int const cap = preyList[pr.p].queen ? 1000 : PACK_SIZE;
        if (used[pr.a] || taken[pr.p] >= cap)
            continue;
        used[pr.a] = 1;
        assignedTo[pr.a] = pr.p;
        taken[pr.p]++;
        if (agents[pr.a].id == me)
            myPrey = pr.p;
    }
    if (myPrey >= 0)
    {
        myPackSize = taken[myPrey];
        // Rank in the pack (by id): each member takes its own side of her.
        int rank = 0;
        for (std::size_t a = 0; a < agents.size(); a++)
            if (assignedTo[a] == myPrey && agents[a].id < me)
                rank++;
        myPackRank = rank;
    }
}

// Pack mates (other than us) within `r` of a cell, from fleet reports.
int mates_near(int cell, int r)
{
    int k = 0;
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 3 && pack_member(id, st.len, st.leader) && manhattan(st.cell, cell) <= r)
            k++;
    return k;
}

// Shortest step sequence that ends in the enemy queen's head. Other dragons
// stand still during our turn; our own body follows us. Steps past the free
// ones cost a segment each, which does not matter: a head-on kills us anyway.
std::vector<int> regicide_path(int qcell, int maxSteps)
{
    set_body(body, L);
    std::unordered_map<int, int> dist, par, pdir;
    std::vector<int> q{body[0]};
    dist[body[0]] = 0;
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (dc >= maxSteps)
            continue;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n < 0 || dist.count(n))
                continue;
            if (n == qcell)
            {
                std::vector<int> dirs{d};
                for (int x = c; x != body[0]; x = par[x])
                    dirs.push_back(pdir[x]);
                std::reverse(dirs.begin(), dirs.end());
                return dirs;
            }
            if (!vis[n] || (occId[n] >= 0 && occId[n] != me) || !passable(n, dc + 1))
                continue;
            dist[n] = dc + 1;
            par[n] = c;
            pdir[n] = d;
            q.push_back(n);
        }
    }
    return {};
}

// Free cells next to the queen's head for her next move, with our body as it
// will be after a candidate move (newBody, head first; empty = as now).
int queen_exits(int qh, std::vector<int> const& newBody)
{
    int k = 0;
    for (int d = 0; d < 4; d++)
    {
        int n = cross(qh, d);
        if (n == -2)
            n = nb(qh, d); // unexplored: may be open
        if (n < 0)
            continue;
        if (vis[n] && occId[n] >= 0 && occId[n] != me)
            continue; // another dragon (or her own body) is there
        if (std::find(newBody.begin(), newBody.end(), n) != newBody.end())
            continue; // we will be there
        k++;
    }
    return k;
}

// Running guess of the round-500 tiebreaks: queen length, then longest
// dragon, then total length. -1 = unknown.
struct TieState
{
    int ourQ = -1, theirQ = -1, ourLong = 0, theirLong = -1;
};
TieState tie_state()
{
    TieState t;
    if (ourQueenDead)
        t.ourQ = 0;
    else if (me == queenId)
        t.ourQ = L;
    else if (auto const q = fleet.find(queenId); q != fleet.end() && R - q->second.round <= 60)
        t.ourQ = q->second.len;
    if (enemyQueenDead)
        t.theirQ = 0;
    else if (auto const e = enemyLenMem.find(enemyQueenId); e != enemyLenMem.end() && R - e->second.round <= 150)
        t.theirQ = e->second.len;
    t.ourLong = L;
    for (auto const& [id, st] : fleet)
        if (R - st.round <= 60)
            t.ourLong = std::max(t.ourLong, st.len);
    for (auto const& [id, lm] : enemyLenMem)
        if (R - lm.round <= 100)
            t.theirLong = std::max(t.theirLong, lm.len);
    return t;
}

// HARVEST_ADAPT: the round the harvest starts. Our queen no longer than theirs (a tie loses on the next tiebreaks):
// the bigger the gap, the earlier - feeders walk one cell a turn, and each drop is a pearl or two.
int harvest_start()
{
    if (!HARVEST_ADAPT)
        return HARVEST_START;
    static int cacheR = -1, cacheV = HARVEST_START;
    if (cacheR == R)
        return cacheV;
    cacheR = R;
    cacheV = HARVEST_START;
    if (R < HARVEST_EARLIEST || ourQueenDead || enemyQueenDead)
        return cacheV;
    TieState const t = tie_state();
    if (t.ourQ >= 0 && t.theirQ >= 0 && t.ourQ <= t.theirQ)
        cacheV = std::max(HARVEST_EARLIEST, HARVEST_START - 10 - HARVEST_PER_GAP * (t.theirQ - t.ourQ));
    else if (HARVEST_UNKNOWN > 0 && t.ourQ >= 0 && t.theirQ < 0 && t.ourQ < HARVEST_UNKNOWN_GOAL)
        cacheV = std::min(HARVEST_START, HARVEST_UNKNOWN); // strong teams hide a growing queen: assume we are behind
    return cacheV;
}

// Giant (insurance for the longest-dragon tiebreak): our longest non-queen
// leader we know of, -1 if none.
int giant_id()
{
    int best = -1, bestLen = 0;
    if (isLeader && !amQueen)
    {
        best = me;
        bestLen = L;
    }
    for (auto const& [id, st] : fleet)
        if (id != queenId && st.leader && R - st.round <= 30 && (st.len > bestLen || (st.len == bestLen && id < best)))
        {
            best = id;
            bestLen = st.len;
        }
    return best;
}

// Is our queen already long enough? Past max(QUEEN_FEED_CAP, their queen's
// known length + margin) feeding her more only adds risk.
bool queen_fed_enough()
{
    TieState const t = tie_state();
    if (t.ourQ < 0)
        return false;
    int const cap = std::max(QUEEN_FEED_CAP, t.theirQ >= 0 ? t.theirQ + QUEEN_FEED_MARGIN : 0);
    return t.ourQ >= cap;
}

// Does a 1-wide corridor lead from `start` to `goal` (within maxSteps)?
// Walks only corridor cells (<= 2 open sides) and stops at visible dragons,
// so it finds the queen's head from the side she is facing.
bool corridor_leads_to(int start, int goal, int maxSteps)
{
    std::unordered_map<int, int> dist{{start, 0}};
    std::vector<int> q{start};
    for (std::size_t i = 0; i < q.size(); i++)
    {
        int const c = q[i], dc = dist[c];
        if (c == goal)
            return true;
        if (dc >= maxSteps || (c != start && open_sides(c) > 2))
            continue;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(c, d);
            if (n < 0 || dist.count(n))
                continue;
            if (n != goal && vis[n] && occId[n] >= 0)
                continue;
            dist[n] = dc + 1;
            q.push_back(n);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// QUEEN_LOOK (v5.0.2): the queen's lookahead in contact.
// ---------------------------------------------------------------------------
// Every enemy moves after her each turn. lookT[c] = the first turn some enemy can have its head on c: within its
// free + paid reach (a segment per paid step, down to length 2) in turn 1, then its free steps per turn after that
// (conservative: paying would shorten it). Reported enemies count with their age. She dies at turn k on a cell with
// lookT <= k. Her lines are walked to LOOK_DEPTH single steps (known water, bodies in view are walls, her own tail
// moves on); a line is worth (turns survived, then ground she reaches strictly before every enemy and her fewest safe
// exits along the way). One-turn danger stays with the threat map; this ranks what comes after.
struct LookRes
{
    bool ran = false;
    std::array<int, 4> surv{};
    std::array<double, 4> val{};
};
std::vector<int> lookT, lookD, lookQ, lookRel;
int lookS = 0;
std::vector<int> lookStamp;

void look_enemy(int src, int f, int budget1)
{
    // budget1: cells it can cover in turn 1 (free + paid); f per turn after that
    int const maxR = std::min(48, budget1 + f * LOOK_DEPTH + 2);
    lookS++;
    lookQ.clear();
    lookQ.push_back(src);
    lookStamp[src] = lookS;
    lookD[src] = 0;
    for (std::size_t i = 0; i < lookQ.size(); i++)
    {
        int const c = lookQ[i], dc = lookD[c];
        int const t = dc <= budget1 ? 1 : 1 + (dc - budget1 + f - 1) / f;
        if (dc > 0)
            lookT[c] = std::min(lookT[c], t);
        if (dc >= maxR)
            continue;
        for (int k = 0; k < 4; k++)
        {
            int const n = step_open(c, k);
            if (n < 0 || lookStamp[n] == lookS)
                continue;
            if (vis[n] && occId[n] >= 0 && occId[n] != me)
                continue; // bodies in view block it (ours: the queen's own body moves on)
            lookStamp[n] = lookS;
            lookD[n] = dc + 1;
            lookQ.push_back(n);
        }
    }
}

LookRes queen_lookahead(std::array<Cand, 4> const& cands)
{
    LookRes res;
    if (!QUEEN_LOOK || body.empty())
        return res;
    int const hd = body[0];
    bool contact = false;
    for (auto const& h : heads)
        if (!h.mine && manhattan(h.cell, hd) <= LOOK_NEAR)
            contact = true;
    if (!contact)
        return res;
    if (static_cast<int>(lookT.size()) != N)
    {
        lookT.assign(N, INF);
        lookD.assign(N, 0);
        lookStamp.assign(N, 0);
        lookRel.assign(N, 0);
    }
    std::fill(lookT.begin(), lookT.end(), INF);
    std::unordered_set<int> seenIds;
    for (auto const& h : heads)
    {
        if (h.mine)
            continue;
        seenIds.insert(h.id);
        int const len = std::max(h.len, h.estLen);
        int const f = std::max(1, (len + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f + (PAID_REACH ? std::max(0, len - 2) : 0));
        if (manhattan(h.cell, hd) <= m + f * LOOK_DEPTH + 2)
            look_enemy(h.cell, f, std::max(f, m));
    }
    for (auto const& e : enemyReps)
    {
        int const age = R - e.round;
        if (age < 1 || age > REP_MAX_AGE || seenIds.count(e.id))
            continue;
        int const el = std::max(e.len, e.estLen);
        int const f = std::max(1, (el + 3) / 4);
        int const m = std::min(PAID_REACH_CAP, f * (age + 1) + (PAID_REACH ? std::max(0, el - 2) : 0));
        if (manhattan(e.cell, hd) <= m + f * LOOK_DEPTH + 2)
            look_enemy(e.cell, f, m);
    }
    res.ran = true;

    // Free and safe at turn t: known water, no body in view (other than hers), and no enemy there by turn t.
    auto other_body = [](int n) { return vis[n] && occId[n] >= 0 && occId[n] != me; };
    std::vector<int> b;
    int len = 0;
    // Ground she reaches strictly first from the leaf (turn base), her body releasing as she moves.
    auto territory = [&](int base) {
        int const cap = 2 * len + 8;
        lookS++;
        for (int i = 0; i < static_cast<int>(b.size()) && i < len; i++)
        {
            lookStamp[b[i]] = lookS;
            lookRel[b[i]] = len - i; // free once she has moved this many steps
        }
        lookQ.clear();
        lookQ.push_back(b[0]);
        lookD[b[0]] = 0;
        lookRel[b[0]] = 0;
        int cnt = 0;
        for (std::size_t i = 0; i < lookQ.size() && cnt < cap; i++)
        {
            int const c = lookQ[i], dc = lookD[c];
            if (dc >= cap)
                continue;
            for (int k = 0; k < 4; k++)
            {
                int const n = cross(c, k);
                if (n < 0 || other_body(n) || base + dc + 1 >= lookT[n])
                    continue;
                if (lookStamp[n] == lookS && (lookRel[n] == 0 || dc + 1 < lookRel[n]))
                    continue; // seen already, or her own body still there
                lookStamp[n] = lookS;
                lookRel[n] = 0;
                lookD[n] = dc + 1;
                lookQ.push_back(n);
                cnt++;
            }
        }
        return std::min(1.0, static_cast<double>(cnt) / cap);
    };
    auto safe_exits = [&](int turn) {
        int ex = 0;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(b[0], k);
            if (n < 0 || other_body(n) || turn + 1 >= lookT[n])
                continue;
            bool own = false;
            for (int i = 0; i + 1 < len && i < static_cast<int>(b.size()); i++)
                own |= b[i] == n;
            ex += !own;
        }
        return ex;
    };
    // Depth-first over her lines: returns the best (survived, value) from here.
    std::function<std::pair<int, double>(int, int)> walk = [&](int turn, int minEx) -> std::pair<int, double> {
        if (turn >= lookT[b[0]])
            return {turn - 1, 0.0};
        int const ex = safe_exits(turn);
        minEx = std::min(minEx, ex);
        if (turn == LOOK_DEPTH)
            return {turn, LOOK_TERR_W * territory(turn) + LOOK_EXIT_W * std::min(minEx, 3) / 3.0};
        std::pair<int, double> best{turn, -1.0}; // stuck: alive this turn, nowhere to go next
        bool any = false;
        for (int k = 0; k < 4; k++)
        {
            int const n = cross(b[0], k);
            if (n < 0 || other_body(n))
                continue;
            bool own = false;
            for (int i = 0; i + 1 < len && i < static_cast<int>(b.size()); i++)
                own |= b[i] == n;
            if (own)
                continue;
            any = true;
            std::vector<int> const saved = b;
            b.insert(b.begin(), n);
            if (static_cast<int>(b.size()) > len)
                b.pop_back();
            auto const r = walk(turn + 1, minEx);
            b = saved;
            if (r.first > best.first || (r.first == best.first && r.second > best.second))
                best = r;
        }
        if (!any)
            return {turn, 0.0};
        return best;
    };
    for (auto const& cd : cands)
    {
        res.surv[cd.d] = 0;
        res.val[cd.d] = 0.0;
        if (!cd.legal || cd.blind || cd.dest < 0 || cd.nb1.empty())
            continue;
        len = L + (cd.eat ? 1 : 0);
        b.assign(cd.nb1.begin(), cd.nb1.begin() + std::min<int>(cd.nb1.size(), len));
        auto const r = walk(1, 4);
        res.surv[cd.d] = r.first;
        res.val[cd.d] = std::max(0.0, r.second);
    }
    return res;
}

void decide(Controller& ct)
{
    int const head = body[0];
    int const facing = dir_index(ct.get_dir());
    claimTarget = -1;
    if (amQueen && ourQueenSpawn < 0)
        ourQueenSpawn = head;
    if (L > lastLenSeen)
        lastEatR = R; // grew: ate something
    lastLenSeen = L;
    // Starving queen: she is (almost) the only eater and has not eaten for a
    // while. Her portal / unexplored / loop rules would leave her at length
    // 2-3 for ever (no split, no fleet), so with no enemy in view they relax.
    bool enemyInView = false;
    for (auto const& h : heads)
        if (!h.mine)
            enemyInView = true;
    bool const queenStarving = STARVE_RELAX && amQueen && units <= 2 && R >= 30 && R - lastEatR >= STARVE_ROUNDS && !enemyInView && !lateSafe;
    queenStarvingG = queenStarving;
    headHist.push_back(head);
    if (headHist.size() > 16)
        headHist.erase(headHist.begin());
    // First look: is the queen in a pocket that kelp closes off inside her
    // view, with no portal? A split child there could never get out, so she
    // keeps her length and just circles (she needs one free cell to move).
    if (QUEEN_SEALED_NOSPLIT && amQueen && !sealedChecked)
    {
        sealedChecked = true;
        bool closed = true;
        std::vector<int> reg{head};
        std::unordered_set<int> inReg{head};
        for (std::size_t i = 0; i < reg.size() && closed; i++)
            for (int d = 0; d < 4 && closed; d++)
            {
                std::uint8_t const et = edgeType[edge_code(reg[i], d)];
                if (et == E_KELP)
                    continue;
                if (et != E_OPEN)
                {
                    closed = false; // portal or unexplored edge: not sealed
                    break;
                }
                int const n = nb(reg[i], d);
                if (!vis[n])
                {
                    closed = false; // the pocket runs out of view
                    break;
                }
                if (inReg.insert(n).second)
                    reg.push_back(n);
            }
        queenSealed = closed;
        sealedCells = closed ? static_cast<int>(reg.size()) : 0;
    }
    bool const sealedNoSplit = amQueen && queenSealed && L <= sealedCells - 1;
    if (ourQueenSpawn < 0 && R <= 1)
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                ourQueenSpawn = h.cell;
    if (HUNTERS && ourQueenSpawn >= 0 && (R % 20 == me % 20 || (amQueen && R < 5)))
        queue_fact(msg_qspawn(ourQueenSpawn), amQueen ? 60 : 40, 1);
    if (EQ_DEATH_DETECT && enemyQueenDead && R % 8 == me % 8)
        queue_fact(msg_queen_dead(true), 55, 1); // keep telling: relays are lossy
    if (EQ_DEATH_DETECT && ourQueenDead && R % 8 == (me + 4) % 8)
        queue_fact(msg_queen_dead(false), 55, 1);

    if (amQueen && QUEEN_THREAT_MAP)
    {
        compute_queen_threat();
        for (int c = 0; c < N; c++)
            if (qThreatVis[c] == 0 && !(vis[c] && occId[c] >= 0))
                contested[c] = 1;
    }
    set_body(body, L);
    plan_bfs(head, distHead);

    // ---- Candidate moves ------------------------------------------------
    std::array<Cand, 4> cands;
    int const cap = std::min(N, 3 * L + 24);
    bool anyLegal = false, anySafe = false, attackAvail = false;
    int attackDir = -1, attackLen = 0;
    for (int d = 0; d < 4; d++)
    {
        Cand& cd = cands[d];
        cd.d = d;
        int const dest = cross(head, d);
        cd.dest = dest;
        if (dest == -1 || dest == -2)
            continue;
        if (dest == -3)
        {
            cd.legal = true;
            cd.blind = true;
            anyLegal = true;
            continue;
        }
        if (vis[dest] && occId[dest] >= 0 && occId[dest] != me)
        {
            if (occHead[dest] && !occMine[dest])
            {
                cd.attack = true;
                attackAvail = true;
                attackDir = d;
                for (auto const& h : heads)
                    if (h.cell == dest)
                        attackLen = h.len;
            }
            continue;
        }
        if (!passable(dest, 1) || (vis[dest] && occId[dest] == me))
            continue;
        cd.legal = true;
        anyLegal = true;
        cd.eat = pearl_at(dest);
        std::vector<int> nb1;
        nb1.reserve(body.size() + 1);
        nb1.push_back(dest);
        nb1.insert(nb1.end(), body.begin(), body.end());
        int const nl = L + (cd.eat ? 1 : 0);
        if (!cd.eat && static_cast<int>(body.size()) >= L)
            nb1.pop_back();
        int const tailCell = static_cast<int>(nb1.size()) >= nl ? nb1.back() : -1;
        set_body(nb1, nl);
        FloodRes f1 = flood(dest, tailCell, cap);
        cd.area = f1.area + (f1.frontier && !amQueen ? nl : 0); // the queen trusts only known water
        // A snake cannot turn round in a corridor, so raw area is only trusted
        // when there is room to circle; reaching our own tail is always an exit.
        bool const frontierExit = FRONTIER_OPEN && !isLeader; // small dragons may explore narrow passages
        cd.ok1 = f1.tail || cd.area >= 2 * nl + 4 || (frontierExit && f1.frontier);

        // Depth 2: is there a follow-up that keeps us free?
        for (int d2 = 0; d2 < 4 && !cd.ok2; d2++)
        {
            int const n2 = cross(dest, d2);
            if (n2 < 0)
                continue;
            set_body(nb1, nl);
            if (!passable(n2, 1) || contested[n2] || (vis[n2] && occId[n2] == me && ownStamp[n2] != stamp))
                continue;
            bool const eat2 = pearl_at(n2);
            std::vector<int> nb2{n2};
            nb2.insert(nb2.end(), nb1.begin(), nb1.end());
            int const nl2 = nl + (eat2 ? 1 : 0);
            if (!eat2 && static_cast<int>(nb1.size()) >= nl)
                nb2.pop_back();
            int const tail2 = static_cast<int>(nb2.size()) >= nl2 ? nb2.back() : -1;
            set_body(nb2, nl2);
            FloodRes const f2 = flood(n2, tail2, cap);
            if (f2.tail || f2.area + (f2.frontier && !amQueen ? nl2 : 0) >= 2 * nl2 + 2 || (frontierExit && f2.frontier))
                cd.ok2 = true;
        }
        if (amQueen && cd.ok2 && !queen_can_survive(nb1, nl))
            cd.ok1 = cd.ok2 = false; // a corridor with no way back
        // Pocket check: a portal box or a small walled area can look roomy to
        // the flood (it counts water beyond the box's other portals, and our
        // tail as an exit). Can the body really keep moving in there?
        if (POCKET_CHECK && !amQueen && cd.ok2)
        {
            bool const viaPortal = dest != nb(head, d);
            bool const smallEnclosed = !f1.frontier && f1.area < 3 * nl;
            if ((viaPortal || smallEnclosed) && !queen_can_survive(nb1, nl, POCKET_NODE_CAP))
                cd.ok1 = cd.ok2 = false; // a trap - the dead-end dive logic decides if its pearls are worth it
        }
        // TOPO_TRAP: a dead-end branch of any shape (a tree of corridors, a room with no loop longer than us).
        if (TOPO_TRAP && !amQueen && cd.ok1 && topo_trap(head, d, nl))
            cd.ok1 = cd.ok2 = false;
        if (QUEEN_LOOP_SAFE && amQueen)
            cd.noLoop = !loop_reachable(nb1, nl);
        if (cd.ok2)
            anySafe = true;
        cd.nb1 = std::move(nb1);
    }
    // Dead ends with pearls (non-queens): worth it when the pearls pay for
    // the survival split at the bottom (the user's compensation rule).
    double bestDive = -1e9;
    // Each dive node floods ~2L+4 cells, so the node budget shrinks with
    // length: a long dragon costs no more per candidate than a short one.
    diveCap = DIVE_NODE_CAP * 12 / std::max(12, 2 * L + 4);
    // Long dragons never mine dead ends (a giant in a pocket is a giant lost),
    // and their dive search is the costliest thing a turn can do.
    if (DIVE_ENABLE && !amQueen && L <= DIVE_MAX_LEN)
        for (auto& cd : cands)
            if (cd.legal && !cd.blind && !cd.ok1 && !cd.nb1.empty() && !contested[cd.dest])
            {
                diveNodes = 0;
                std::vector<int> ec;
                if (cd.eat)
                    ec.push_back(cd.dest);
                cd.dive = dive_rec(cd.nb1, L + (cd.eat ? 1 : 0), 1, cd.eat ? 1 : 0, ec);
                bestDive = std::max(bestDive, cd.dive);
            }
    // Pocket mining: with a teammate close by, a dive that only breaks even is
    // worth it - the stub left inside recycles into pearls for the teammate.
    bool mateNearHead = false;
    for (auto const& h : heads)
        if (h.mine && h.id != me && manhattan(h.cell, head) <= 5)
            mateNearHead = true;
    double const diveMin = mateNearHead && !amQueen ? DIVE_MATE_GAIN : DIVE_MIN_GAIN;
    bool const diving = bestDive >= diveMin;
    // The queen with no safe move: still tell a corridor that leads on from a
    // known dead end (unexplored water counts as a way on here).
    if (QUEEN_SURV_RANK && amQueen && !anySafe)
        for (auto& cd : cands)
            if (cd.legal && !cd.blind && !cd.nb1.empty())
            {
                ignoreContested = false;
                std::vector<int> const& b1 = cd.nb1;
                set_body(b1, L + (cd.eat ? 1 : 0));
                pstamp++;
                survNodes = 0;
                pathStamp[b1[0]] = pstamp;
                pathTime[b1[0]] = 0;
                int const len1 = L + (cd.eat ? 1 : 0);
                cd.surv = survive_rec(b1[0], 0, std::min(2 * len1 + 6, 50), len1);
            }
    set_body(body, L);
#ifdef FLEET_DEBUG
    {
        std::string dbg = "head " + std::to_string(head % W) + "," + std::to_string(head / W) + " body" + std::to_string(body.size()) + "/" + std::to_string(L);
        for (auto const& cd : cands)
        {
            int const code = edge_code(head, cd.d);
            dbg += std::string(" ") + DCH[cd.d] + ":dest" + std::to_string(cd.dest) + " et" + std::to_string(edgeType[code]) +
                   " oe" + std::to_string(obsEdge[code]) + " L" + std::to_string(cd.legal) + " ok" + std::to_string(cd.ok1) + std::to_string(cd.ok2);
            if (cd.dest >= 0)
                dbg += " occ" + std::to_string(occId[cd.dest]) + " own" + std::to_string(ownStamp[cd.dest] == stamp) + "/" + std::to_string(ownRel[cd.dest]);
            if (cd.dive > -1e9)
                dbg += " dive" + std::to_string(cd.dive).substr(0, 4);
        }
        for (auto const& h : heads)
            dbg += " H" + std::to_string(h.id) + (h.mine ? "m@" : "e@") + std::to_string(h.cell % W) + "," + std::to_string(h.cell / W) + "l" + std::to_string(h.len);
        for (int tc : enemyTails)
            dbg += " T@" + std::to_string(tc % W) + "," + std::to_string(tc / W);
        ct.output_log(dbg);
    }
#endif

    bool const nextToEnemy = is_adjacent_enemy_head(head);
    bool queenAdjacent = false;
    for (auto const& cd : cands)
        if (cd.attack && occId[cd.dest] == enemyQueenId && !enemyQueenDead)
        {
            queenAdjacent = true;
            attackDir = cd.d;
        }
    bool const allIn = ourQueenDead && !enemyQueenDead; // we lose at round 500 unless their queen dies
    // v2.14 anti-escort: an enemy head that stays within 2 of our queen for a
    // few rounds is stalking her (runs beside her along a wall until she is
    // boxed). The queen reports it; any short teammate strikes it.
    int qNow = -1;
    if (!ourQueenDead)
    {
        if (amQueen)
            qNow = head;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qNow = h.cell;
        auto const qf = fleet.find(queenId);
        if (qNow < 0 && qf != fleet.end() && R - qf->second.round <= 1)
            qNow = qf->second.cell;
    }
    std::unordered_set<int> stalkers;
    if (ANTI_ESCORT && qNow >= 0)
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, qNow) <= 2)
            {
                auto& es = escortSeen[h.id];
                if (es.first != R)
                {
                    es.second = es.first == R - 1 ? es.second + 1 : 1;
                    es.first = R;
                }
                if (es.second >= ESCORT_ROUNDS)
                    stalkers.insert(h.id);
            }
    if (amQueen)
        for (auto const& h : heads)
            if (stalkers.count(h.id))
            {
                queue_fact(msg_danger(h.cell, 1), 88, 2);
                queue_fact(msg_enemy(h.cell, h.id, h.len), 86, 2);
            }
    int stalkerDir = -1;
    for (auto const& cd : cands)
        if (cd.attack && stalkers.count(occId[cd.dest]))
            stalkerDir = cd.d;
    // A head-on kills both dragons. While expanding, numbers are what we are
    // building, so only trade up: the enemy queen, a longer enemy, or when we
    // clearly outnumber them.
    bool const evenTrades = !(EXPANSION && expansionPhase);
    // v2.14: their dragons rarely attack ours, so a 1-for-1 only helps a swarm
    // that out-produces us. Trade up only (or with a clear majority).
    // Not outnumbered (by the scaled count): even trades as before (an
    // opponent that attacks our dragons must be answered).
    bool const tradeUp = !TRADE_UP || allIn || majorityHunt || attackLen >= TRADE_UP_RATIO * L || (!outnumbered && !survivalMode);
    bool const wantAttack = attackAvail && !amQueen &&
                            ((queenAdjacent) || (stalkerDir >= 0 && !isLeader) ||
                             (!isLeader && tradeUp && (role == ROLE_COMBAT || mode == MODE_COMBAT || allIn) &&
                              (evenTrades ? (L <= attackLen + 1 || units > est_enemy_units() + 1 || L <= 4 || allIn)
                                          : (attackLen > L || units > est_enemy_units() + 1 || allIn)) &&
                              // Swarms out-produce us: a 1-for-1 trade only pays while we are ahead.
                              (!SAFE_TRADES || allIn || (!survivalMode && (attackLen > L || !outnumbered)))));
    if (!queenAdjacent && stalkerDir >= 0 && !isLeader)
        attackDir = stalkerDir;
    int const freeN = std::min(free_steps(), MAX_SPRINT);

    // ---- 0. Sprint into the enemy queen ("one smells blood") -----------------
    if (PAID_REGICIDE && !amQueen && !enemyQueenDead)
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(head, 1), 70, 2);
                    if (EQ_DEATH_DETECT)
                        queue_fact(msg_queen_dead(true), 99, 4); // certain: she moved before us
                    return act_moves(ct, path, "regicide");
                }
            }
    if (ANTI_ESCORT && !amQueen && !isLeader && L <= 8 && !stalkers.empty())
        for (auto const& h : heads)
            if (!h.mine && stalkers.count(h.id))
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 75, 2);
                    return act_moves(ct, path, "escort-hit");
                }
            }
    // Snipe: a short dragon that can put its head on an enemy head this turn
    // (free + paid steps) and is no longer than it strikes: a head-on kills
    // both, so it never trades down. Witnesses report the death drops.
    if (SNIPE && !amQueen && !isLeader && L <= SNIPE_MAX_LEN &&
        (!SAFE_TRADES || (!survivalMode && units >= est_enemy_units() + 4 && units >= 8)))
    {
        std::vector<HeadInfo const*> targets;
        for (auto const& h : heads)
            if (!h.mine && h.id != enemyQueenId && (majorityHunt || (TRADE_UP && outnumbered ? h.len >= TRADE_UP_RATIO * L : h.len >= L)))
                targets.push_back(&h);
        std::sort(targets.begin(), targets.end(), [](HeadInfo const* a, HeadInfo const* b) { return a->len > b->len; });
        int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
        for (auto const* h : targets)
        {
            std::vector<int> const path = regicide_path(h->cell, reach);
            set_body(body, L);
            if (!path.empty())
            {
                queue_fact(msg_danger(h->cell, 1), 70, 2);
                return act_moves(ct, path, "snipe");
            }
        }
    }
    // Pack members: work out the shared prey, and strike a long enemy dragon
    // when its head is within reach (a short dragon for a long one).
    bool const packer = HUNTERS && !amQueen && !isLeader && !isGuard && (isHunter || (ENDGAME_PACKS && ourQueenDead && L <= 6)) &&
                        (allIn || !(SAFE_TRADES && (survivalMode || outnumbered)));
    if (packer)
    {
        assign_prey();
        int const preyId = myPrey >= 0 ? preyList[myPrey].id : -1;
        for (auto const& h : heads)
            if (!h.mine && h.id != enemyQueenId && (h.id == preyId || h.len >= std::max(PREY_MIN_LEN, L + 4)))
            {
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 70, 2);
                    return act_moves(ct, path, "pack-strike");
                }
            }
    }
    if (!amQueen && !enemyQueenDead && !queenAdjacent)
    {
        bool near = false;
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId && manhattan(h.cell, head) <= std::min(freeN + 1, 3))
                near = true;
        if (near)
        {
            SprintPlan kp;
            std::vector<int> dirs, eatenCells;
            sprint_dfs(body, L, 0, std::min(freeN + 1, 3), freeN, 0, dirs, eatenCells, false, 0, kp);
            set_body(body, L);
            if (kp.kill)
            {
                queue_fact(msg_danger(head, 1), 70, 2);
                if (EQ_DEATH_DETECT)
                    queue_fact(msg_queen_dead(true), 99, 4);
                return act_moves(ct, kp.dirs, "regicide");
            }
        }
    }

    // Short teammates strike enemy heads that have our queen within their
    // reach (one of ours for one attacker; the queen moves first and cannot
    // dodge a sprint).
    if (STALKER_INTERCEPT && !amQueen && !isLeader && !ourQueenDead && L <= 6)
    {
        int qc = -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qc = h.cell;
        auto const q = fleet.find(queenId);
        if (qc < 0 && q != fleet.end() && R - q->second.round <= 2)
            qc = q->second.cell;
        if (qc >= 0)
            for (auto const& h : heads)
            {
                if (h.mine)
                    continue;
                int const el = std::max(h.len, h.estLen);
                if (manhattan(h.cell, qc) > (el + 3) / 4 + std::max(0, el - 2) + 1)
                    continue;
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(h.cell, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(h.cell, 1), 75, 2);
                    return act_moves(ct, path, "intercept");
                }
            }
    }
    // The queen: every single step ends inside some enemy's reach. Sprint out
    // with free steps, paying segments if needed (length is cheaper than her).
    if (QUEEN_PAID_ESCAPE && QUEEN_HARD_THREAT && QUEEN_THREAT_MAP && amQueen && anyLegal)
    {
        double bestSingle = 1e18;
        for (auto const& cd : cands)
            if (cd.legal && !cd.blind && cd.dest >= 0)
                bestSingle = std::min(bestSingle, queen_threat_pen(cd.dest));
        if (bestSingle >= 0.5 * HARD_THREAT_PEN)
        {
            SprintPlan ep;
            std::vector<int> dirs, eatenCells;
            int const steps = std::min(4, free_steps() + std::max(0, L - 2));
            if (steps >= 2)
                sprint_dfs(body, L, 0, steps, free_steps(), 0, dirs, eatenCells, false, 0, ep);
            set_body(body, L);
            if (!ep.dirs.empty() && ep.value > -0.5 * HARD_THREAT_PEN)
            {
                int el = 0;
                std::vector<int> const endBody = simulate_steps(body, L, ep.dirs, el);
                bool const ok = !endBody.empty() && queen_can_survive(endBody, el) &&
                                !(QUEEN_LOOP_SAFE && !loop_reachable(endBody, el));
                set_body(body, L);
                if (ok)
                    return act_moves(ct, ep.dirs, "q-escape");
            }
        }
    }

    // ---- 1. Death is certain: survival split (sections 3, 12) --------------
    if (!anyLegal)
    {
        // Boxed in by our own body: split so that the blocking segment becomes
        // the child's tail. The child moves later this round, which frees that
        // cell, and the front part keeps as many segments as possible.
        if (static_cast<int>(body.size()) >= L)
        {
            int bestKeep = -1;
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(head, d);
                if (n < 0)
                    continue;
                for (int i = 2; i <= L - 2; i++)
                    if (body[i] == n && i > bestKeep)
                        bestKeep = i;
            }
            if (bestKeep >= 2 && split_allowed(ct, L - bestKeep) && (!amQueen || bestKeep >= L - 2 || bestKeep >= 4 || L < 6))
                return act_split(ct, L - bestKeep, "unwind-split");
        }
        // Stalling only helps if something around the head will move away:
        // another dragon, or our own last two segments (they leave as the child).
        bool willClear = false;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            if (n < 0)
                continue;
            if (otherRel[n])
                willClear = true;
            for (int i = std::max(0, static_cast<int>(body.size()) - 2); i < static_cast<int>(body.size()); i++)
                if (body[i] == n && static_cast<int>(body.size()) >= L)
                    willClear = true;
        }
        if (amQueen && willClear && L >= 4 && split_allowed(ct, 2))
        {
            // Splitting is not a move: the queen keeps its head, survives this
            // turn and keeps all but 2 segments (the child takes the tail).
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, 2, "queen-stall");
        }
        if (attackAvail && !isLeader && L < 6 && !(QUEEN_NO_SUICIDE && amQueen && L >= 4 && split_allowed(ct, L - 2)))
            return act_move(ct, attackDir, "last-attack");
        if (L >= 4 && split_allowed(ct, L - 2))
        {
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, L - 2, "save-split");
        }
        if (attackAvail)
            return act_move(ct, attackDir, "last-attack");
        // Die without taking a teammate along: never into a friendly head
        // (least of all the queen's); kelp, bodies or our own neck are harmless.
        int deathDir = -1, deathRank = 99;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            int rank = 2;
            if (n >= 0 && vis[n] && occId[n] >= 0 && occHead[n] && occMine[n])
                rank = occId[n] == queenId ? 9 : 8;
            else if (n == -3 || (n >= 0 && !vis[n]))
                rank = 0; // unknown far side: might even survive
            else if (n >= 0 && occId[n] >= 0 && !occMine[n])
                rank = 1; // enemy body
            if (rank < deathRank)
            {
                deathRank = rank;
                deathDir = d;
            }
        }
        return act_move(ct, deathDir >= 0 ? deathDir : facing, "doomed");
    }
    if (!anySafe && !diving && (freeN >= 2 || L >= 5))
    {
        SprintPlan ep;
        std::vector<int> dirs, eatenCells;
        sprint_dfs(body, L, 0, std::min(freeN + (L >= 5 ? 1 : 0), 3), freeN, 0, dirs, eatenCells, false, 0, ep);
        set_body(body, L);
        bool escOk = ep.kill || ep.value > (amQueen && QUEEN_THREAT_MAP ? -1e6 : QUEEN_EXIT_GUARD ? -0.5 * QUEEN_EXIT_PEN : -1e17);
        if (escOk && amQueen && QUEEN_LOOP_SAFE && !ep.kill && !ep.dirs.empty())
        {
            int el = 0;
            std::vector<int> const endBody = simulate_steps(body, L, ep.dirs, el);
            if (!endBody.empty() && !loop_reachable(endBody, el))
                escOk = false; // an escape into a pocket only delays it
            set_body(body, L);
        }
        if (escOk)
            return act_moves(ct, ep.dirs, ep.kill ? "regicide" : "escape");
    }
    if (!anySafe && !diving && !amQueen && L >= 4 && split_allowed(ct, L - 2) && child_can_escape(L - 2))
    {
        // Trapped but a pearl is one step away: eat it first (compensation),
        // the survival split is still available next turn.
        bool anyBlind = false, pearlFirst = false;
        for (auto const& cd : cands)
        {
            anyBlind |= cd.blind;
            pearlFirst |= cd.legal && cd.eat;
        }
        if (!anyBlind && !pearlFirst)
        {
            queue_fact(msg_danger(head, nextToEnemy ? 1 : 2), 80, 3);
            return act_split(ct, L - 2, "trap-split");
        }
    }

    // Insurance: the queen is about to be struck - every move ends inside an
    // enemy's reach and a visible enemy head is within its free steps of
    // her. She splits off all but 2: if she dies, the length lives on in the
    // child (the tail end, away from the striker) instead of vanishing.
    if (QUEEN_INSURE && !QUEEN_NO_SUICIDE && amQueen && anyLegal && QUEEN_THREAT_MAP && L >= QUEEN_INSURE_LEN && static_cast<int>(body.size()) >= L)
    {
        bool allHit = true, closeStriker = false;
        for (auto const& cd : cands)
            if (cd.legal && (cd.blind || cd.dest < 0 || qThreat[cd.dest] >= INF))
                allHit = false;
        for (auto const& h : heads)
            if (!h.mine)
            {
                int const el = std::max(h.len, h.estLen);
                if (manhattan(h.cell, head) <= (el + 3) / 4 + 1)
                    closeStriker = true;
            }
        if (allHit && closeStriker && split_allowed(ct, L - 2) && child_can_escape(L - 2))
        {
            queue_fact(msg_danger(head, 1), 80, 3);
            return act_split(ct, L - 2, "insure-split");
        }
    }

    // Recycle: boxed in for good (every move leaves less room than our own
    // length) with a teammate close by. The drop is the same either way;
    // dying now, next to the teammate, means it actually gets eaten.
    if (RECYCLE && !anySafe && !diving && !amQueen && !isLeader && L >= 3 && body.size() >= 2 && (!SURV_ECON || units >= RECYCLE_MIN_UNITS))
    {
        bool boxed = true, pearlFirst = false;
        for (auto const& cd : cands)
        {
            if (!cd.legal)
                continue;
            if (cd.blind || cd.ok1 || cd.area >= L)
                boxed = false;
            pearlFirst |= cd.eat;
        }
        bool mateNear = false;
        for (auto const& h : heads)
            if (h.mine && h.id != me)
                for (int i = 0; i < L && i < static_cast<int>(body.size()); i++)
                    if (manhattan(h.cell, body[i]) <= RECYCLE_MATE_DIST)
                        mateNear = true;
        if (boxed && !pearlFirst && mateNear)
            for (int d = 0; d < 4; d++)
                if (nb(head, d) == body[1])
                {
                    queue_fact(msg_danger(head, 2), 60, 1);
                    return act_move(ct, d, "recycle");
                }
    }

    // ---- 2. Head-on attack (sections 15, 16) ------------------------------
    if (wantAttack)
    {
        queue_fact(msg_danger(head, 1), 70, 2);
        return act_move(ct, attackDir, "attack");
    }

    // ---- Target ----------------------------------------------------------
    // Cap feeding: once the fleet is at its cap, more pearls cannot become
    // dragons, so the spare ones go to the queen - only while she is safe
    // (known position, no enemy reported near her lately) and we are not in
    // survival mode. Self-throttling: a feed drops the fleet below the cap.
    // Sonar rarely reaches a guard, so a guard that sees her also works it out:
    // late in the game (or once a bunker is likely), a length-3 queen whose
    // whole body sits inside one 2x2 box two rounds running is bunkered there.
    if (BUNKER_GUARDS && !amQueen && !ourQueenDead && (lateSafe || enemyQueenDead || R >= TIE_BUNKER_ROUND))
        for (auto const& h : heads)
            if (h.mine && h.id == queenId && h.len == 3)
            {
                std::vector<int> qcells;
                for (int c = 0; c < N; c++)
                    if (vis[c] && occId[c] == queenId)
                        qcells.push_back(c);
                if (static_cast<int>(qcells.size()) != 3)
                    break;
                int const tls[4] = {h.cell, nb(h.cell, 3), nb(h.cell, 0), nb(nb(h.cell, 3), 0)};
                std::array<int, 4> bx{};
                for (int tl : tls)
                {
                    if (!box_cells(tl, bx))
                        continue;
                    bool all = true;
                    for (int c : qcells)
                        if (std::find(bx.begin(), bx.end(), c) == bx.end())
                            all = false;
                    if (!all)
                        continue;
                    if (inferTl == tl && inferR == R - 1)
                    {
                        qBunkerTl = tl;
                        qBunkerR = R;
                    }
                    inferTl = tl;
                    inferR = R;
                    break;
                }
            }
    // The queen is bunkered (announced or seen within the last 5 rounds): her box.
    std::array<int, 4> qBox{};
    bool const bunkerFresh = BUNKER_GUARDS && !amQueen && !ourQueenDead && qBunkerTl >= 0 && R - qBunkerR <= 5 && box_cells(qBunkerTl, qBox);
    int postDir = -1;
    bool capFeed = false;
    // Open maps only: on crowded maps the feeders' bodies wall her in.
    if (CAP_FEED && openMap && !ourQueenDead && !survivalMode && R >= CAP_FEED_ROUND && !room_for_more())
    {
        int qk = amQueen ? head : -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qk = h.cell;
        auto const qf = fleet.find(queenId);
        if (qk < 0 && qf != fleet.end() && R - qf->second.round <= 30)
            qk = qf->second.cell;
        if (qk >= 0)
        {
            capFeed = !queen_fed_enough();
            for (auto const& e : enemyReps)
                if (R - e.round <= 10 && manhattan(e.cell, qk) <= 10)
                    capFeed = false;
        }
    }
    std::vector<PearlCand> const pearls = gather_pearls();
    int target = -1;
    int portalDir = -1;
    char const* why = "explore";
    int pi = choose_pearl(pearls);
    // Commitment: switching pearls every other turn wastes the moves made so
    // far (in a maze the two pearls are often in different corridors).
    if (TARGET_COMMIT && lastPearl >= 0 && (!amQueen || QUEEN_COMMIT))
    {
        int prevIdx = -1;
        for (int k = 0; k < static_cast<int>(pearls.size()); k++)
            if (pearls[k].cell == lastPearl)
                prevIdx = k;
        if (prevIdx >= 0 && prevIdx != pi && distHead[lastPearl] < INF)
        {
            int const dPrev = distHead[lastPearl];
            int const dNew = pi >= 0 ? distHead[pearls[pi].cell] : INF;
            bool const clearlyBetter = pi >= 0 && dNew + COMMIT_MARGIN <= dPrev && dNew * 10 <= dPrev * 7;
            if (!clearlyBetter)
                pi = prevIdx;
        }
    }
    if (pi >= 0)
    {
        target = pearls[pi].cell;
        claimTarget = target;
        claimEta = distHead[target];
        why = "pearl";
    }
    // Last rounds: a living queen beats any pearl (their queen is often dead
    // already and we cannot know it). Only pearls right next to her that no
    // enemy can reach; otherwise she keeps to the ground she is on.
    // v2.14 lead safe: ahead on queen length from LEAD_ROUND (their queens are
    // short and hidden), so a pearl trip can only lose the game.
    bool leadSafe = false;
    if (LEAD_SAFE && amQueen && !ourQueenDead && !enemyQueenDead && R >= LEAD_ROUND)
    {
        TieState const lt = tie_state();
        leadSafe = (lt.theirQ >= 0 && L > lt.theirQ) || (lt.theirQ < 0 && L >= LEAD_MIN_LEN);
    }
    if (amQueen && (lateSafe || leadSafe) && target >= 0)
    {
        bool risky = distHead[target] > 3 || (QUEEN_THREAT_MAP && qThreat[target] < INF);
        for (auto const& e : enemyReps)
            if (R - e.round <= 10 && manhattan(e.cell, target) <= 8)
                risky = true;
        if (FEED_FIX && QUEEN_THREAT_MAP && pearl_at(target) && distHead[target] <= FEED_EAT_DIST && qThreat[target] >= INF)
            risky = false; // a drop right by her that no enemy can reach this turn
        if (risky)
        {
            target = -1;
            claimTarget = -1;
            why = "late-safe";
        }
    }
    if (QUEEN_EVADE && QUEEN_HARD_THREAT && amQueen && target >= 0 && (qThreat[target] < INF || qMargin[target] <= 1))
    {
        target = -1;
        claimTarget = -1;
        why = "evade";
    }

    // Combat dragons hunt enemies, preferring the ones near our leaders. When
    // we clearly outnumber them, small dragons also hunt the long enemies.
    bool const outnumber = units > enemyEst + 3 && distinctEnemies > 0;
    bool const hunter = !isLeader && ((mode == MODE_COMBAT && role == ROLE_COMBAT) || (outnumber && L <= 5));
    // v2.12 gate, also for this chase: never while outnumbered or in survival mode.
    bool const huntOk = allIn || !(HUNT_GATE && (survivalMode || outnumbered));
    // Majority on a smaller map: every spare dragon hunts, any enemy will do.
    bool const majHunter = majorityHunt && !amQueen && !isLeader && !isGuard;
    if ((hunter && huntOk) || majHunter ? (target < 0 || distHead[target] > (majHunter ? 2 : 4)) : false)
    {
        int best = -1, bestD = majHunter ? 24 : 16;
        for (auto const& e : enemyReps)
        {
            if (R - e.round > 3)
                continue;
            if (!majHunter && (TRADE_UP && outnumbered ? e.len < std::max(6, TRADE_UP_RATIO * L) : (!(mode == MODE_COMBAT && role == ROLE_COMBAT) && e.len < 6)))
                continue;
            int dd = dist_to(distHead, e.cell);
            if (majHunter && dd < INF)
            {
                // One hunter per enemy: leave it to a teammate that is nearer
                // (several converging on one target run into each other).
                bool nearerMate = false;
                for (auto const& h : heads)
                    if (h.mine && h.id != me && h.id != queenId && manhattan(h.cell, e.cell) < manhattan(head, e.cell))
                        nearerMate = true;
                for (auto const& [id, st] : fleet)
                    if (id != me && id != queenId && R - st.round <= 2 && manhattan(st.cell, e.cell) + 1 < manhattan(head, e.cell))
                        nearerMate = true;
                if (nearerMate)
                    continue;
            }
            for (auto const& [id, s] : fleet)
                if (s.leader && R - s.round <= 10 && manhattan(s.cell, e.cell) <= 6)
                    dd -= 6;
            if (dd < bestD)
            {
                bestD = dd;
                best = e.cell;
            }
        }
        if (best >= 0)
        {
            target = best;
            why = "hunt";
        }
    }

    // The enemy queen decides the game: chase her when she is close, always
    // for combat dragons, and with everyone once our own queen is gone.
    if (!amQueen && !enemyQueenDead && (!isLeader || allIn))
    {
        int qc = -1, qround = -1;
        for (auto const& e : enemyReps)
            if (e.id == enemyQueenId && R - e.round <= 4 && e.round > qround)
            {
                qc = e.cell;
                qround = e.round;
            }
        if (qc >= 0)
        {
            int const dq = dist_to(distHead, qc);
            int const range = allIn ? 60 : HUNT_QUEEN_RANGE;
            if (dq <= range && (dq <= 6 || allIn || (huntOk && R >= OPEN_FARM_ROUND && (role == ROLE_COMBAT || hunter))))
            {
                target = qc;
                why = "hunt-queen";
            }
        }
    }

    // Farming endgame (section 13): feeders walk to our queen (or, without
    // her, to the nearest leader).
    int feedLeader = -1, feedId = -1;
    // When feeding starts: open maps turn numbers into length from
    // CONSOL_ROUND; behind on queen length we start earlier (sooner still on
    // open maps); otherwise only the last FEED_WINDOW rounds.
    TieState const ts = TIE_TRACK ? tie_state() : TieState{};
    bool const behindQ = TIE_TRACK && !ourQueenDead && !enemyQueenDead &&
                         ((ts.ourQ >= 0 && ts.theirQ >= 0 && ts.ourQ <= ts.theirQ) ||
                          (QUEEN_HOME && R >= LEAD_ROUND && ts.ourQ <= 3) ||
                          (FEED_FIX && R >= LEAD_ROUND && ts.theirQ < 0 && ts.ourQ >= 0 && ts.ourQ < FEED_UNKNOWN_GOAL)); // v2.14: a home queen eats little (unknown counts as short)
    int feedStart = consolidate ? (DYN_ALLOC ? switchRound : CONSOL_ROUND) : 500 - FEED_WINDOW;
    if (behindQ)
        feedStart = std::min(feedStart, openMap ? FEED_BEHIND_OPEN : FEED_BEHIND_CONGESTED);
    if (capFeed)
        feedStart = std::min(feedStart, R); // the fleet is full: its spare pearls go to the queen
    // SIZE_CAP_PLAN: open maps grow until CAP_HARVEST, then every dragon (length 2 too) feeds the largest.
    bool const harvest = SIZE_CAP_PLAN && capOpen && R >= CAP_HARVEST;
    if (SIZE_CAP_PLAN && capOpen)
        feedStart = CAP_HARVEST;
    bool const earlyFeed = R < 500 - FEED_WINDOW; // guards, hunters and defenders keep their jobs until the last rounds
    // A length-2 dragon cannot feed (it would leave one pearl and could not
    // even squeeze into its neck usefully): it would only crowd her.
    bool const feedPhase = !(SIZE_CAP_PLAN && capMode) && !isLeader && (L >= 3 || harvest) && R >= feedStart && mode != MODE_COMBAT && localThreat == 0 &&
                           !(isGuard && bunkerFresh) && // her posts come first
                           !(majorityHunt && earlyFeed) && // winning on numbers: keep hunting until the last rounds
                           !(earlyFeed && ((isGuard && !(DYN_ALLOC && switched && !isScreen)) || isHunter || isDefender)) && !(FEED_ONLY_IF_CONTEST && enemyQueenDead && !ourQueenDead);
    if (feedPhase)
    {
        int bestD = INF, longestId = -1, longestLen = -1;
        // Giant: on open maps a share of the feeders grows it too (it wins
        // the longest-dragon tiebreak if the queens end up dead or tied).
        bool const queenHarvest = CAP_FIX && HARVEST && R >= harvest_start() && !ourQueenDead && !enemyQueenDead; // all of it is hers
        int const gid = GIANT && (consolidate || (GIANT_ALL && R >= GIANT_ROUND)) && !ourQueenDead && !queenHarvest && (me % GIANT_SHARE == 0) ? giant_id() : -1;
        if (gid >= 0 && gid != me)
        {
            for (auto const& h : heads)
                if (h.mine && h.id == gid)
                {
                    feedLeader = h.cell;
                    feedId = gid;
                }
            auto const g = fleet.find(gid);
            if (feedLeader < 0 && g != fleet.end() && R - g->second.round <= 30)
            {
                feedLeader = g->second.cell;
                feedId = gid;
            }
        }
        if (ourQueenDead && R >= 500 - LATE_LONGEST_ONLY)
            for (auto const& [id, s] : fleet)
                if (s.leader && R - s.round <= 5 && s.len > longestLen)
                {
                    longestLen = s.len;
                    longestId = id;
                }
        // A queen long enough to win on length gets no more: the extra length
        // only makes her harder to keep alive. Late on open maps the giant
        // takes it instead; otherwise the dragon keeps its length.
        bool const queenFull = !ourQueenDead && !queenHarvest && queen_fed_enough();
        if (queenFull && feedId < 0 && consolidate)
        {
            int const g2 = giant_id();
            auto const g = fleet.find(g2);
            if (g2 >= 0 && g2 != me && g != fleet.end() && R - g->second.round <= 30)
            {
                feedLeader = g->second.cell;
                feedId = g2;
            }
        }
        for (auto const& [id, s] : fleet)
        {
            if (feedId >= 0 || queenFull)
                break; // feeding the giant, or nobody
            bool const ok = ourQueenDead ? s.leader && (longestId < 0 || id == longestId) : id == queenId;
            if (ok && R - s.round <= (DYN_ALLOC && switched ? 100 : consolidate || capFeed ? 30 : 5) && manhattan(s.cell, head) < bestD)
            {
                bestD = manhattan(s.cell, head);
                feedLeader = s.cell;
                feedId = id;
            }
        }
        for (auto const& h : heads)
            if (h.mine && !queenFull && !(gid >= 0 && feedId == gid) && (ourQueenDead ? (fleet.count(h.id) && fleet[h.id].leader && (longestId < 0 || h.id == longestId)) : h.id == queenId))
            {
                feedLeader = h.cell;
                feedId = h.id;
            }
        // v2.14: she keeps to her home, so without a fresh report go there and
        // look (the sacrifice itself waits until she is in view).
        if (QUEEN_HOME && feedLeader < 0 && feedId < 0 && !ourQueenDead && !queenFull && ourQueenSpawn >= 0 && distHead[ourQueenSpawn] < INF)
        {
            feedLeader = ourQueenSpawn;
            feedId = queenId;
        }
        // Open map, nowhere to find her: the longest leader in view grows instead.
        if (consolidate && feedLeader < 0)
        {
            int bestLen = 0;
            for (auto const& h : heads)
                if (h.mine && h.id != me && fleet.count(h.id) && fleet[h.id].leader && h.len > bestLen)
                {
                    bestLen = h.len;
                    feedLeader = h.cell;
                    feedId = h.id;
                }
        }
        // Queue: a swarm of feeders walls her in, so with enough of them
        // around her already the rest wait their turn further out.
        int crowd = 0;
        if (feedLeader >= 0)
            for (auto const& h : heads)
                if (h.mine && h.id != me && h.id != feedId && manhattan(h.cell, feedLeader) <= 5)
                    crowd++;
        bool const waitTurn = feedLeader >= 0 && crowd >= FEED_CROWD && manhattan(head, feedLeader) > 4;
        if (feedLeader >= 0 && !waitTurn && (target < 0 || distHead[target] > 6))
        {
            target = feedLeader;
            why = "feed";
        }
    }

    // Packs: go for the shared prey; the first one there waits a little for
    // a mate so the pack arrives together (alone, it just goes in).
    if (packer && !feedPhase && (L >= HUNTER_LEN || ourQueenDead) && myPrey >= 0 &&
        (target < 0 || std::string(why) != "pearl" || distHead[target] > 2))
    {
        Prey const& pr = preyList[myPrey];
        if (dist_to(distHead, pr.cell) < INF)
        {
            target = pr.cell;
            why = pr.queen ? "hunter" : "pack";
            bool preyInView = false;
            for (auto const& h : heads)
                if (!h.mine && h.id == pr.id)
                    preyInView = true;
            // Lurk: her position is fresh but she is out of our view. Wait just
            // outside her 7x7 view, each pack member on its own side; the
            // moment she steps toward one she is in its view and reach, and the
            // regicide sprint fires. Two sides: she cannot step away from both.
            int lurkCell = -1;
            if (LURK && pr.queen && !preyInView)
            {
                int qc = -1, qr = -1;
                for (auto const& e : enemyReps)
                    if (e.id == enemyQueenId && e.round > qr)
                    {
                        qr = e.round;
                        qc = e.cell;
                    }
                if (qc >= 0 && R - qr <= LURK_FRESH)
                {
                    static int const off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    int const r = myPackRank % 4;
                    int const ix = ((qc % W + off[r][0] * LURK_DIST) % W + W) % W;
                    int const iy = ((qc / W + off[r][1] * LURK_DIST) % H + H) % H;
                    int const ideal = cell_of(ix, iy);
                    int bestScore = INF;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2 + std::abs(dy); dx <= 2 - std::abs(dy); dx++)
                        {
                            int const c = cell_of(((ix + dx) % W + W) % W, ((iy + dy) % H + H) % H);
                            if (distHead[c] >= INF || cheb(c, qc) < LURK_DIST)
                                continue;
                            int const sc = distHead[c] + 2 * manhattan(c, ideal);
                            if (sc < bestScore)
                            {
                                bestScore = sc;
                                lurkCell = c;
                            }
                        }
                }
            }
            if (lurkCell >= 0)
            {
                target = lurkCell;
                why = "lurk";
            }
            else if (PACK_REGROUP > 0 && myPackSize >= 2 && !preyInView && manhattan(head, pr.cell) <= PACK_REGROUP &&
                     mates_near(pr.cell, PACK_REGROUP + 3) == 0)
            {
                target = -1;
                why = "regroup";
            }
        }
    }

    // Guards: intercept enemies closing on the queen, otherwise screen her
    // from a few tiles out (crowding her walls her in).
    int guardQ = -1;
    if (isGuard && !feedPhase)
    {
        int qCellG = -1;
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCellG = h.cell;
        auto const q = fleet.find(queenId);
        if (qCellG < 0 && q != fleet.end() && R - q->second.round <= (isScreen ? 60 : 3))
            qCellG = q->second.cell;
        if (qCellG < 0 && bunkerFresh)
            qCellG = qBunkerTl; // she is in her announced box
        if (qCellG < 0 && BUNKER_GUARDS && lateSafe && q != fleet.end() && R - q->second.round <= 60)
            qCellG = q->second.cell; // last rounds: find her (she bunkers), then post
        guardQ = qCellG;
        if (qCellG >= 0)
        {
            int threat = -1, threatD = INF;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, qCellG) <= GUARD_INTERCEPT && manhattan(h.cell, qCellG) < threatD)
                {
                    threat = h.cell;
                    threatD = manhattan(h.cell, qCellG);
                }
            if (threat >= 0)
            {
                // A head-on kills both: one guard for one attacker near the queen.
                int const reach = std::min(REGICIDE_MAX, free_steps() + std::max(0, L - 2));
                std::vector<int> const path = regicide_path(threat, reach);
                set_body(body, L);
                if (!path.empty())
                {
                    queue_fact(msg_danger(threat, 1), 75, 2);
                    return act_moves(ct, path, "intercept");
                }
                target = threat;
                why = "intercept";
            }
            else if (bunkerFresh)
            {
                // Post: a 2x2 next to her box covering the most of its open
                // sides, not held by a teammate. Circled at length <= 3 it
                // keeps 3 of its 4 cells filled: no enemy reaches her there.
                std::vector<int> expo;
                for (int c : qBox)
                    for (int d = 0; d < 4; d++)
                    {
                        int const n = cross(c, d);
                        if (n >= 0 && std::find(qBox.begin(), qBox.end(), n) == qBox.end() &&
                            std::find(expo.begin(), expo.end(), n) == expo.end())
                            expo.push_back(n);
                    }
                std::array<int, 4> best{};
                int bestTl = -1;
                double bestScore = -1e18;
                std::array<int, 4> pb{};
                for (int e : expo)
                {
                    int const tls[4] = {e, nb(e, 3), nb(e, 0), nb(nb(e, 3), 0)};
                    for (int tl : tls)
                    {
                        if (!box_cells(tl, pb))
                            continue;
                        bool bad = false;
                        int cover = 0, dmin = INF;
                        for (int c : pb)
                        {
                            if (std::find(qBox.begin(), qBox.end(), c) != qBox.end())
                                bad = true; // never on her box
                            if (vis[c] && occId[c] >= 0 && occId[c] != me && !(occMine[c] && occId[c] == queenId))
                                bad = true; // someone is there (a teammate holds it, or an enemy)
                            if (pearlState[c] == 1 && std::find(body.begin(), body.end(), c) == body.end())
                                bad = true; // a pearl in the loop would make a length-3 guard 4
                            if (std::find(expo.begin(), expo.end(), c) != expo.end())
                                cover++;
                            dmin = std::min(dmin, c == head ? 0 : distHead[c]);
                        }
                        for (auto const& [id, st] : fleet)
                            if (id != me && id != queenId && R - st.round <= 2 && std::find(pb.begin(), pb.end(), st.cell) != pb.end())
                                bad = true; // reported holding it
                        if (bad || dmin >= INF)
                            continue;
                        double const sc = 100.0 * cover - dmin;
                        if (sc > bestScore)
                        {
                            bestScore = sc;
                            bestTl = tl;
                            best = pb;
                        }
                    }
                }
                if (bestTl >= 0)
                {
                    bool const inPost = std::find(best.begin(), best.end(), head) != best.end();
                    target = -1;
                    why = "post";
                    if (inPost && L >= 4)
                    {
                        int const child = L >= 6 ? L - 3 : L - 2; // keep 3 (2 from length 4-5)
                        if (split_allowed(ct, child) && child_can_escape(child))
                            return act_split(ct, child, "post-trim");
                    }
                    if (inPost && L <= 3)
                    {
                        for (int d = 0; d < 4; d++)
                        {
                            int const n = cross(head, d);
                            if (n < 0 || std::find(best.begin(), best.end(), n) == best.end())
                                continue;
                            if (std::find(body.begin(), body.end(), n) != body.end() || (vis[n] && occId[n] >= 0))
                                continue;
                            if (pearl_at(n) && L >= 3)
                                continue;
                            postDir = d;
                        }
                    }
                    else if (!inPost)
                    {
                        int bd = INF;
                        for (int c : best)
                            if (distHead[c] < bd)
                            {
                                bd = distHead[c];
                                target = c;
                            }
                    }
                }
            }
            else if (isScreen && screen_post(qCellG) >= 0)
            {
                // Stand watch out toward the enemy: attackers are seen (and
                // reported to her) before they are within striking reach.
                int const post = screen_post(qCellG);
                if (manhattan(head, post) > 1)
                {
                    target = post;
                    why = "screen";
                }
                else if (target >= 0 && distHead[target] > 3)
                    target = -1;
            }
            else if (CHOKE_GUARDS && !isScreen && choke_post(qCellG) >= 0 && distHead[choke_post(qCellG)] < INF && guard_stays_by_her(qCellG))
            {
                // Hold the chokepoint on her way to the enemy side (guards circle near it).
                int const post = choke_post(qCellG);
                if (manhattan(head, post) > 1)
                {
                    target = post;
                    why = "guard";
                }
                else if (target >= 0 && distHead[target] > 3)
                    target = -1;
            }
            else if (manhattan(head, qCellG) > GUARD_RING && distHead[qCellG] < INF)
            {
                target = qCellG;
                why = "guard";
            }
            else
            {
                if (target >= 0 && distHead[target] > 4)
                    target = -1; // stay around here; no long trips while on duty
                // Line up with her: a ping only reaches a dragon on the same
                // row or column, so a guard there can pass her its reports.
                int const qx = qCellG % W, qy = qCellG / W, hx = head % W, hy = head / W;
                if (SCREENS_MORE && target < 0 && hx != qx && hy != qy)
                {
                    int best = -1, bd = INF;
                    for (int axis = 0; axis < 2; axis++)
                        for (int off = -GUARD_RING; off <= GUARD_RING; off++)
                        {
                            if (std::abs(off) < 3)
                                continue;
                            int const c = axis == 0 ? cell_of(((qx + off) % W + W) % W, qy) : cell_of(qx, ((qy + off) % H + H) % H);
                            if (distHead[c] < bd && (!vis[c] || occId[c] < 0))
                            {
                                bd = distHead[c];
                                best = c;
                            }
                        }
                    if (best >= 0 && bd <= 6)
                    {
                        target = best;
                        why = "guard-line";
                    }
                }
            }
        }
    }

    if (guardQ >= 0 && target < 0 && std::string(why) != "post")
        why = "guard"; // idle on station: no exploring away from the queen

    // v2.14 rescue: a stalker reported next to our queen (or seen): short
    // teammates nearby come and strike it (one of ours for her life).
    if (ANTI_ESCORT && !amQueen && !isLeader && L <= 8 && !ourQueenDead && !feedPhase && std::string(why) != "intercept" &&
        std::string(why) != "post")
    {
        auto const qf = fleet.find(queenId);
        int const qc = qNow >= 0 ? qNow : (qf != fleet.end() && R - qf->second.round <= 3 ? qf->second.cell : -1);
        if (qc >= 0)
        {
            int best = -1, bd = RESCUE_RANGE + 1;
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 1 && manhattan(dg.cell, qc) <= 3)
                {
                    int const dd = dist_to(distHead, dg.cell);
                    if (dd < bd)
                    {
                        bd = dd;
                        best = dg.cell;
                    }
                }
            for (auto const& h : heads)
                if (!h.mine && stalkers.count(h.id) && distHead[h.cell] < bd)
                {
                    bd = distHead[h.cell];
                    best = h.cell;
                }
            if (best >= 0)
            {
                target = best;
                why = "rescue";
            }
        }
    }

    // Once the enemy queen is dead ours only has to stay alive: no long
    // trips for pearls, the guards come to her.
    if (GUARDS && amQueen && enemyQueenDead && target >= 0 && distHead[target] > 3)
        target = -1;

    // Bunker: head for the chosen box, then circle it.
    // Tie bunker: late in the game with both queens short and theirs no
    // longer than ours, queen length is a tie at best and the next tiebreak
    // is the longest dragon, so ours just circles at her length (<= 3).
    bool tieBunker = false;
    if (TIE_BUNKER && BUNKER && amQueen && !enemyQueenDead && !ourQueenDead && R >= TIE_BUNKER_ROUND && L <= 3)
    {
        auto const it = enemyLenMem.find(enemyQueenId);
        tieBunker = TIE_FORCE || (it != enemyLenMem.end() && it->second.exact && R - it->second.round <= TIE_SEEN_ROUNDS && it->second.len <= L);
    }
    // CAP_FIX: in the harvest rounds a drop right by her (no enemy can reach it) beats circling at her length:
    // the feeders die for it. Their queen known dead keeps the full bunker.
    int harvestDrop = -1;
    if (CAP_FIX && HARVEST && amQueen && R >= harvest_start() && !enemyQueenDead && !ourQueenDead && QUEEN_THREAT_MAP)
        for (int c = 0; c < N; c++)
            if (vis[c] && pearl_at(c) && distHead[c] <= FEED_EAT_DIST && qThreat[c] >= INF &&
                (harvestDrop < 0 || distHead[c] < distHead[harvestDrop]))
                harvestDrop = c;
    if (harvestDrop >= 0)
        tieBunker = false;
    bool const lateBunker = LATE_SAFE && BUNKER && amQueen && lateSafe && L <= 3 && !ourQueenDead && harvestDrop < 0; // circle at her length
    bool const bunkerMode = BUNKER && amQueen && (enemyQueenDead || tieBunker || lateBunker) && !ourQueenDead;
    int bunkerDir = -1;
    bool inBox = false;
    if (bunkerMode)
    {
        std::array<int, 4> bx{};
        if (bunkerBox < 0 || !box_cells(bunkerBox, bx) || !box_usable(bx))
            bunkerBox = choose_bunker();
        if (bunkerBox >= 0 && box_cells(bunkerBox, bx))
        {
            inBox = std::find(bx.begin(), bx.end(), head) != bx.end();
            why = "bunker";
            target = -1;
            if (inBox && L <= 3)
            {
                for (int d = 0; d < 4; d++)
                {
                    int const n = cross(head, d);
                    if (n < 0 || std::find(bx.begin(), bx.end(), n) == bx.end())
                        continue;
                    if (std::find(body.begin(), body.end(), n) != body.end())
                        continue; // the tail still blocks the cell it leaves
                    if (vis[n] && occId[n] >= 0)
                        continue;
                    if (pearl_at(n) && L >= 3)
                    {
                        bunkerBox = -1; // a pearl in the loop would jam it: pick another box next turn
                        continue;
                    }
                    bunkerDir = d;
                }
            }
            else if (!inBox)
            {
                int bd = INF;
                for (int x : bx)
                    if (distHead[x] < bd)
                    {
                        bd = distHead[x];
                        target = x;
                    }
            }
        }
    }

    // v2.14 rest: with nothing safe to eat she waits in open water near home
    // (drifting along walls with no goal is how queens end up in corners).
    if (harvestDrop >= 0 && !bunkerMode && target < 0)
    {
        target = harvestDrop; // CAP_FIX: the feeders died for it
        why = "harvest-eat";
    }
    if (QUEEN_HOME && amQueen && !bunkerMode && target < 0 && !queenStarving && ourQueenSpawn >= 0)
    {
        if (restCell < 0 || R % 25 == 0 || (vis[restCell] && occId[restCell] >= 0 && occId[restCell] != me) || distHead[restCell] >= INF)
            restCell = choose_rest();
        if (restCell >= 0 && manhattan(head, restCell) > 1 && distHead[restCell] < INF)
        {
            target = restCell;
            why = "rest";
        }
    }
    // v2.14 home: with nothing to eat inside her leash the queen walks back
    // home (their stalkers hunt queens that roam).
    if (QUEEN_HOME && amQueen && !bunkerMode && target < 0 && !queenStarving && ourQueenSpawn >= 0 && manhattan(head, ourQueenSpawn) > queenLeash + 4 && queenLeash < INF &&
        distHead[ourQueenSpawn] < INF)
    {
        target = ourQueenSpawn;
        why = "home";
    }

    // Portals: whole regions (and most of the pearls on some maps) lie behind
    // portals whose far end nobody has seen. Scouts open them early: one
    // crossing teaches everyone the pair, and paths then use it.
    // Only worth the risk when the portals lead somewhere we cannot walk to:
    // our reachable area (unexplored edges count as open) is a small part of
    // the map.
    int reachable = 0;
    for (int c = 0; c < N; c++)
        reachable += distHead[c] < INF;
    bool const boxedIn = reachable * 100 < SCOUT_REACH_PCT * N;
    int const scoutMaxLen = POCKET_CHECK ? BLIND_PORTAL_MAX_LEN : 12;
    bool const scout = PORTAL_SCOUTS && (boxedIn || PORTAL_ALWAYS) && !amQueen && !isLeader && L <= scoutMaxLen && (hunterSlot || role == ROLE_PORTAL) &&
                       (why == std::string("pearl") || why == std::string("explore"));
    if (!amQueen && !isLeader && L <= scoutMaxLen && (scout || (target < 0 && (boxedIn || role == ROLE_PORTAL))) &&
        !(isGuard && why != std::string("explore")))
    {
        int pd = -1;
        int const pg = choose_portal_goal(pd);
        bool take = pg >= 0 && (target < 0 || std::string(why) != "pearl" || distHead[pg] + SCOUT_PEARL_MARGIN <= distHead[target]);
        if (!PORTAL_SCOUTS)
            take = take && target < 0 && role == ROLE_PORTAL;
        if (take)
        {
            target = pg;
            portalDir = pd;
            why = "portal";
        }
    }
    if (AGGRO_EXPANSION && target < 0 && hunterSlot && expansionPhase && !isLeader && !isGuard && !(SAFE_TRADES && (survivalMode || outnumbered)))
    {
        // Take the middle early: idle hunter-group dragons head for the enemy
        // side (the mirror image of our queen's start), past the centre.
        int const qt = enemyQueenDead ? -1 : enemy_queen_target();
        if (qt >= 0 && distHead[qt] < INF)
        {
            target = qt;
            why = "push";
        }
    }
    if (isDefender && !feedPhase && std::string(why) != "intercept" &&
        (target < 0 || halfMargin(target) <= 0 || distHead[target] > 10))
    {
        int const dg = choose_camp_goal(true);
        if (dg >= 0)
        {
            target = dg;
            why = "defend";
        }
    }
    // Opening: pearls appear on spawn tiles, so an idle dragon waits by one
    // rather than wandering off to explore (we lost most opening pearl races).
    if (target < 0 && R < OPEN_CAMP_ROUNDS && !amQueen && !isGuard && std::string(why) == "explore")
    {
        int const cg = choose_camp_goal();
        if (cg >= 0 && distHead[cg] <= 20)
        {
            target = cg;
            why = "camp";
        }
    }
    if (target < 0 && !(GUARDS && amQueen && enemyQueenDead) && !(amQueen && QUEEN_AVOID_UNKNOWN && !queenStarving) && std::string(why) != "guard" &&
        std::string(why) != "regroup" && std::string(why) != "evade")
    {
        target = choose_explore_goal();
        // Nothing unseen left to look at: wait by a spawn tile instead of
        // revisiting old ground (pearls appear there, not on the way).
        if (IDLE_CAMP && !amQueen && !isGuard && (target < 0 || seenRound[target] >= 0))
        {
            int const cg = choose_camp_goal();
            if (cg >= 0)
            {
                target = cg;
                why = "camp";
            }
        }
    }

    // v2.14 loop stall: circling one 2x2 for rounds without eating while the
    // pearl stays out of reach (a maze target that keeps flipping): drop it.
    if (LOOP_STALL && std::string(why) == "pearl" && target >= 0 && distHead[target] > 3 && R - lastEatR >= LOOP_STALL_ROUNDS &&
        static_cast<int>(headHist.size()) >= LOOP_STALL_ROUNDS)
    {
        bool tight = true;
        for (std::size_t i = headHist.size() - LOOP_STALL_ROUNDS; i < headHist.size() && tight; i++)
            for (std::size_t j = i + 1; j < headHist.size(); j++)
                if (manhattan(headHist[i], headHist[j]) > 2)
                    tight = false;
        if (tight)
            skipPearl[target] = R + 30;
    }
    // Stalled: no step closer to this pearl for STALL_ROUNDS turns (it is
    // behind kelp we keep running into): leave it to others for a while.
    if (std::string(why) == "pearl" && target >= 0)
    {
        int const dNow = distHead[target];
        if (target != stallTarget)
        {
            stallTarget = target;
            stallBest = dNow;
            stallRound = R;
        }
        else if (dNow < stallBest)
        {
            stallBest = dNow;
            stallRound = R;
        }
        else if (STALL_ROUNDS > 0 && R - stallRound >= STALL_ROUNDS)
        {
            skipPearl[target] = R + STALL_SKIP;
            stallTarget = -1;
        }
        lastPearl = target;
    }
    else
        lastPearl = -1;

    // ---- HARVEST (v2.14.9): the last rounds turn our nearby dragons into length for the queen ------------
    // (or, with her dead, for our longest dragon: ties to the lowest id; that dragon itself never dies for it).
    if (HARVEST && R >= harvest_start() && !amQueen && L >= 2 && body.size() >= 2 && !(CAP_FIX && enemyQueenDead && !ourQueenDead))
    {
        int hId = -1, hCell = -1;
        bool hSeen = false;
        if (!ourQueenDead)
        {
            hId = queenId;
            for (auto const& h : heads)
                if (h.mine && h.id == queenId)
                {
                    hCell = h.cell;
                    hSeen = true;
                }
            auto const q = fleet.find(queenId);
            if (hCell < 0 && q != fleet.end() && R - q->second.round <= 30)
                hCell = q->second.cell; // she keeps to her home: an older report is still close
            if (hCell < 0 && ourQueenSpawn >= 0)
                hCell = ourQueenSpawn; // she keeps to her home
        }
        else
        {
            int bestLen = L;
            hId = me;
            for (auto const& [id, st] : fleet)
                if (id != me && R - st.round <= 10 && (st.len > bestLen || (st.len == bestLen && id < hId)))
                {
                    bestLen = st.len;
                    hId = id;
                    hCell = st.cell;
                }
            for (auto const& h : heads)
                if (h.mine && h.id == hId)
                {
                    hCell = h.cell;
                    hSeen = true;
                }
        }
        int pd = INF; // path steps from our head to the target's head
        if (hCell >= 0)
        {
            pd = distHead[hCell];
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(hCell, d);
                if (n >= 0 && distHead[n] < INF)
                    pd = std::min(pd, distHead[n] + 1);
            }
        }
        if (hId != me && hCell >= 0 && pd <= HARVEST_RADIUS)
        {
            bool safe = true; // dominated: no enemy head near the target or next to us
            for (auto const& h : heads)
                if (!h.mine && (manhattan(h.cell, hCell) <= HARVEST_SAFE || manhattan(h.cell, head) <= 3))
                    safe = false;
            for (auto const& e : enemyReps)
                if (R - e.round <= 2 && manhattan(e.cell, hCell) <= HARVEST_SAFE)
                    safe = false;
            if (safe)
            {
                if (hSeen && cheb(head, hCell) <= HARVEST_DIE_DIST)
                {
                    for (int d = 0; d < 4; d++)
                        if (nb(head, d) == body[1])
                            return act_move(ct, d, "harvest"); // into our neck: the body becomes pearls beside her
                }
                target = hCell;
                why = "feed"; // walk to her (feeders are allowed close to the queen)
            }
        }
    }

    // ---- 3. Feeding sacrifice (section 13) ----------------------------------
    if (feedPhase && feedLeader >= 0 && (L >= 3 || harvest) && units > (earlyFeed ? cons_floor() : 2) && feedId != me)
    {
        // In view: die within 3 cells of her head (the drop lands where she
        // can eat it in a move or two). Out of view: a fresh report within 4.
        bool inView = false, close = false;
        for (auto const& h : heads)
            if (h.mine && h.id == feedId)
            {
                inView = true;
                close = cheb(h.cell, head) <= 3;
                if (FEED_FIX)
                {
                    // P4: the drop must be on her side of the kelp - a short water path to her head.
                    int pd = INF;
                    for (int d = 0; d < 4; d++)
                    {
                        int const n = cross(h.cell, d);
                        if (n >= 0 && distHead[n] < INF)
                            pd = std::min(pd, distHead[n] + 1);
                    }
                    close = pd <= FEED_PATH;
                }
            }
        if (!inView && !FEED_FIX) // P4: out of view we cannot tell which side of the kelp she is on
        {
            auto const ls = fleet.find(feedId);
            close = ls != fleet.end() && R - ls->second.round <= 2 && cheb(ls->second.cell, head) <= 4;
        }
        // Squeeze into the neck: the body turns into pearls next to the leader.
        if (close && body.size() >= 2)
            for (int d = 0; d < 4; d++)
                if (nb(head, d) == body[1])
                    return act_move(ct, d, "feed-leader");
    }

    // ---- 4. Splits (sections 2, 8, 14) ---------------------------------------
    // v2.14: inside a dead end an optional split leaves the head part to die
    // long; eat to the bottom, then split all but 2.
    // v2.15 (P1): only a real dead end - every legal move continues into a
    // 1-wide corridor that ends. "No safe move" alone is common in corridor
    // mazes and crowds, and blocking splits there stalled the whole fleet.
    if (anySafe)
        miningTrip = false; // back in open water: the trip is over
    bool inDeadEnd = FOUNTAINS && !amQueen && anyLegal && !anySafe;
    // A break-even trip (2 pearls in, a 2-cell stub left) only pays by
    // splitting inside: that is how a lone miner turns into a fleet.
    if (DEADEND_STRICT && !miningTrip)
        inDeadEnd = false;
    if (inDeadEnd && DEADEND_STRICT)
        for (auto const& cd : cands)
            if (cd.legal && (cd.blind || cd.dest < 0 || cd.dest != nb(head, cd.d) || !pocket_ahead(head, cd.dest)))
                inDeadEnd = false; // a way on that is not a dead end (or a portal): splits stay allowed
    bool const calm = !nextToEnemy && !inDeadEnd;
    // Runners: on big maps a share of the fleet stays long enough for two
    // free steps a turn, covers distance, and buds off children of 3+.
    bool const isRunner = (RUNNERS && !amQueen && !isLeader && !isGuard && !isHunter && role == ROLE_EXPLORER && N >= RUNNER_MIN_CELLS &&
                           growth_open()) ||
                          isDefender; // defenders stay medium length too: a 2-long blocker is easily stepped round
    int const growChild = isRunner ? L - RUNNER_KEEP : L / 2;
    if (amQueen && bunkerMode && enemyQueenDead && !tieBunker && !sealedNoSplit && L >= 4 && (inBox || qThreat[head] == INF))
    {
        int const child = L - 2; // keep 2: one stray pearl then still leaves the loop a free cell
        if (split_allowed(ct, child))
            return act_split(ct, child, "bunker-trim");
    }
    // SIZE_CAP_PLAN: before round X no dragon - the queen included - keeps length 4 or more: split it in two.
    if (SIZE_CAP_PLAN && capMode && L >= 4 && calm && !(amQueen && (bunkerMode || sealedNoSplit)) && room_for_more() &&
        !(CAP_FIX && amQueen && !capOpen && R >= CAP_LATCH_BY)) // CAP_FIX: the length she has by then stays hers
    {
        int const child = L / 2;
        bool enemyClose = false;
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, head) <= 3)
                enemyClose = true;
        // QUEEN_CAP_EXITS: like her own split - the child stays on our tail cells this round, so she needs two ways on
        // that are not dead ends without them (a cap split at round 4 left her boxed in: dead at round 6).
        int realExits = 0;
        for (int d = 0; d < 4; d++)
        {
            int const n = cross(head, d);
            if (n >= 0 && !(vis[n] && occId[n] >= 0) && !(n == nb(head, d) && pocket_ahead(head, n)))
                realExits++;
        }
        bool const ok = amQueen ? anySafe && !enemyClose && (!QUEEN_CAP_EXITS || realExits >= 2) && split_allowed(ct, child) && child_can_escape(child, true) && split_keeps_loop(child)
                                : split_allowed(ct, child) && child_can_escape(child, true);
        if (ok)
            return act_split(ct, child, amQueen ? "cap-queen-split" : "cap-split");
    }
    if (amQueen && !bunkerMode && !sealedNoSplit)
    {
        if (EXPANSION)
        {
            // Numbers first: the queen splits too (head-ons kill whatever the
            // length, so a short queen is no easier to kill and harder to trap).
            bool crowded = false;
            for (auto const& h : heads)
                if (manhattan(h.cell, head) <= 3)
                    crowded = true;
            // v2.14: the child stays on our tail cells this round, so she needs
            // two ways on that are not dead ends without them.
            int realExits = 0;
            for (int d = 0; d < 4; d++)
            {
                int const n = cross(head, d);
                if (n >= 0 && !(vis[n] && occId[n] >= 0) && !(QUEEN_HOME && n == nb(head, d) && pocket_ahead(head, n)))
                    realExits++;
            }
            if (calm && !consolidate && queenSplits && anySafe && !crowded && L >= 4 && (!QUEEN_HOME || (realExits >= 2 && ((ADAPT_SPLIT && queenOpening) || (units < QUEEN_SPLIT_UNITS && L - L / 2 >= QUEEN_MIN_KEEP)))) &&
                room_for_more() &&
                split_allowed(ct, L / 2) && child_can_escape(L / 2, true) && split_keeps_loop(L / 2))
                return act_split(ct, L / 2, "queen-split");
        }
        else if (calm && R < EARLY_SPLIT_ROUNDS && units < QUEEN_SEED_UNITS && L >= 5 && split_allowed(ct, 2) && child_can_escape(2, true) &&
                 split_keeps_loop(2))
            return act_split(ct, 2, "queen-seed");
    }
    else if (calm && !consolidate && (!isHunter || (FAST_OPEN && expansionPhase) || (SPLIT_MORE && R < EARLY_SPLIT_ROUNDS) || (HUNTER_SPLIT_LEN > 0 && L >= HUNTER_SPLIT_LEN)) &&
             L >= (isRunner ? RUNNER_SPLIT_LEN : EARLY_SPLIT_MIN_LEN) && (EXPANSION ? growth_open() && !isLeader : R < EARLY_SPLIT_ROUNDS) &&
             room_for_more() && split_allowed(ct, growChild) && child_can_escape(growChild, true))
        return act_split(ct, growChild, "grow-fleet");

    // Lane split: the tail sits among spawn tiles nobody else holds, so the
    // child is born right where the pearls keep coming.
    if (LANE_SPLIT && !consolidate && calm && !amQueen && !isLeader && !isGuard && (!isHunter || expansionPhase) && L >= 4 &&
        (!isRunner || L - 2 >= RUNNER_KEEP) && units < std::min(unitLimit - 2, std::max(2, N / LANE_DENSITY)) &&
        static_cast<int>(body.size()) >= L && split_allowed(ct, 2))
    {
        int const tail = body.back(), tx = tail % W, ty = tail / W;
        int tiles = 0;
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3 + std::abs(dy); dx <= 3 - std::abs(dy); dx++)
                if (nextSpawn[cell_of((tx + dx + W) % W, (ty + dy + H) % H)] >= 0)
                    tiles++;
        bool mateNear = false;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, tail) <= 4)
                mateNear = true;
        if (tiles >= LANE_TILES && !mateNear && child_can_escape(2, true))
            return act_split(ct, 2, "lane-split");
    }

    if (calm && !consolidate && !isLeader && !isHunter && L >= 4 && (!isRunner || L - 2 >= RUNNER_KEEP) && pi >= 0 && room_for_more() && split_allowed(ct, 2) &&
        static_cast<int>(body.size()) >= L)
    {
        int const tail = body.back();
        std::vector<int> child{tail, body[L - 2]};
        wstamp++;
        for (int i = 0; i < L - 2; i++)
            wallStamp[body[i]] = wstamp;
        set_body(child, 2);
        plan_bfs(tail, distTmp);
        wstamp++;
        set_body(body, L);
        int const pc = pearls[pi].cell;
        int const dt = distTmp[pc], dh = distHead[pc];
        if (dt <= PEARL_BEHIND_MAX && dt + PEARL_BEHIND_GAIN <= dh && child_can_escape(2, true))
            return act_split(ct, 2, "pearl-split");
    }

    // Half-split: mid-game a very long dragon is slow to turn and easy to trap
    // on a crowded map; two of half the length hold more ground. On open maps
    // the leaders keep growing (length decides the tiebreaks there).
    if (HALF_SPLIT && calm && !amQueen && !isHeir && L >= HALF_SPLIT_LEN && growth_open() && (!isLeader || !openMap) &&
        room_for_more() && split_allowed(ct, L / 2) && child_can_escape(L / 2, true))
        return act_split(ct, L / 2, "half-split");

    if (calm && !consolidate && transitioned && !isLeader && !isHunter && mode == MODE_PRESERVE && L >= PRESERVE_SPLIT_LEN && room_for_more() &&
        split_allowed(ct, L / 2) && child_can_escape(L / 2, true))
        return act_split(ct, L / 2, "keep-fleet");

#ifdef FLEET_DEBUG
    {
        std::string dbg = std::string("target ") + why + " " + (target >= 0 ? std::to_string(target % W) + "," + std::to_string(target / W) : "-") +
                          " d" + (target >= 0 ? std::to_string(distHead[target]) : "-") + " pearls";
        for (auto const& p : pearls)
            dbg += " " + std::to_string(p.cell % W) + "," + std::to_string(p.cell / W);
        ct.output_log(dbg);
    }
#endif
    // ---- 5. Best move ------------------------------------------------------
    // Box their queen in: we see her but have no strike this turn. Their
    // queens die when boxed (a body she runs into kills her; theirs even
    // self-destruct). A head-on with her is a trade we always want.
    int eqHead = -1;
    if (BOX_QUEEN && !amQueen && !isLeader && !enemyQueenDead && std::string(why) != "feed" &&
        (allIn || !(SAFE_TRADES && (survivalMode || outnumbered))))
        for (auto const& h : heads)
            if (!h.mine && h.id == enemyQueenId)
                eqHead = h.cell;
    int const eqExitsNow = eqHead >= 0 ? queen_exits(eqHead, body) : 0;
    // Camping: the job tile has not spawned yet and we are close. A pearl
    // (almost) never appears under a body, so wait beside the tile and step
    // on in the spawn round.
    bool const campWait = CAMP_JOB && std::string(why) == "pearl" && pi >= 0 && pi < static_cast<int>(pearls.size()) &&
                          pearls[pi].cell == target && pearls[pi].appear > R && !pearl_at(target) && distHead[target] <= 3;
    // Our queen in a 1-wide corridor: where is she (vision, else fresh sonar)?
    int qCorr = -1;
    if (CORRIDOR_GUARD && !amQueen && !ourQueenDead)
    {
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCorr = h.cell;
        auto const q = fleet.find(queenId);
        if (qCorr < 0 && q != fleet.end() && R - q->second.round <= 2)
            qCorr = q->second.cell;
        if (qCorr >= 0 && (open_sides(qCorr) > 2 || manhattan(qCorr, head) > 14))
            qCorr = -1;
    }
    std::vector<int> teamCells;
    for (int c = 0; c < N; c++)
        if (vis[c] && occId[c] >= 0 && occId[c] != me && occMine[c])
            teamCells.push_back(c);

    // Teammates must not wall the queen in: measure her free space with our
    // body where it is now and where each move would put it.
    int qHead = -1, qLen = 0, qNeed = 0, qBase = 0;
    if (QUEEN_ROOM && !amQueen)
        for (auto const& h : heads)
            if (h.mine && h.id == queenId && manhattan(h.cell, head) <= 5)
            {
                qHead = h.cell;
                auto const q = fleet.find(queenId);
                qLen = std::max(h.len, q != fleet.end() && R - q->second.round <= 3 ? q->second.len : 0);
                qNeed = 2 * qLen + 4;
                set_body(body, L);
                ignoreContested = true;
                qBase = flood(qHead, -1, qNeed + 4).area;
                ignoreContested = false;
            }

    // Give the queen room: where is she (vision, else sonar status)?
    int qCell = -1;
    if (QUEEN_CLEAR > 0 && !amQueen && !ourQueenDead && std::string(why) != "feed")
    {
        for (auto const& h : heads)
            if (h.mine && h.id == queenId)
                qCell = h.cell;
        auto const q = fleet.find(queenId);
        if (qCell < 0 && q != fleet.end() && R - q->second.round <= 2)
            qCell = q->second.cell;
        if (qCell >= 0 && manhattan(qCell, head) <= QUEEN_CLEAR + 4)
            queen_dist_bfs(qCell, QUEEN_CLEAR + 2);
        else
            qCell = -1;
    }

    if (BUNKER_GUARDS && amQueen && bunkerMode && bunkerBox >= 0)
    {
        qBunkerTl = bunkerBox;
        qBunkerR = R;
        queue_fact(msg_bunker(bunkerBox), 72, 1); // guards come and post round her box
    }
    if (bunkerDir < 0 && postDir >= 0)
        bunkerDir = postDir; // a guard circling its post: same forced loop step
    if (bunkerDir >= 0 && cands[bunkerDir].legal)
    {
        cands[bunkerDir].ok1 = cands[bunkerDir].ok2 = true; // the box loop never traps a queen of length <= 3
        cands[bunkerDir].surv = true;
    }
    // QUEEN_LOOK: her lines in contact (not while circling her bunker box).
    LookRes look;
    int lookBest = 0;
    if (QUEEN_LOOK && amQueen && QUEEN_THREAT_MAP && bunkerDir < 0)
    {
        look = queen_lookahead(cands);
        for (auto const& cd : cands)
            if (look.ran && cd.legal && !cd.blind && cd.dest >= 0)
                lookBest = std::max(lookBest, look.surv[cd.d]);
    }
    int bestD = -1;
    double bestScore = -1e18;
    for (auto& cd : cands)
    {
        if (!cd.legal)
            continue;
        double s = cd.d == bunkerDir ? 900000.0 : 0.0;
        if (look.ran && !cd.blind && cd.dest >= 0)
            s += look.val[cd.d] - LOOK_DEATH_PEN * (lookBest - look.surv[cd.d]);
        if (qCell >= 0 && !cd.blind && cd.dest >= 0)
        {
            // Teammates crowding the queen (her children start right behind
            // her) seal her into corridors; getting out of her way is free.
            int const dq = qDist[cd.dest], dnow = qDist[head];
            if (dq <= QUEEN_CLEAR && dq <= dnow)
                s -= QUEEN_CLEAR_PEN * (QUEEN_CLEAR + 1 - dq) * (open_sides(cd.dest) <= 2 ? 2.0 : 1.0);
        }
        if (guardQ >= 0 && !cd.blind && cd.dest >= 0 && std::string(why) != "intercept")
        {
            int const dg = manhattan(guardQ, cd.dest);
            if (dg < GUARD_MIN && dg <= manhattan(guardQ, head))
                s -= GUARD_MIN_PEN * (GUARD_MIN - dg);
        }
        if (qHead >= 0 && !cd.blind && !cd.nb1.empty() && std::string(why) != "feed")
        {
            set_body(cd.nb1, L + (cd.eat ? 1 : 0));
            ignoreContested = true;
            int const qa = flood(qHead, -1, qNeed + 4).area;
            ignoreContested = false;
            if (qa < qNeed && qa < qBase)
                s -= QUEEN_ROOM_PEN * (1.0 + static_cast<double>(qBase - qa) / qNeed);
        }
        if (cd.blind)
        {
            // Unknown far side: only for small, expendable dragons (section 5).
            bool const wanted = role == ROLE_PORTAL || role == ROLE_EXPLORER;
            if (isLeader)
                s = anySafe ? -1e7 : -5e5; // better than a certain trap
            else if (POCKET_CHECK && L > BLIND_PORTAL_MAX_LEN)
                s = anySafe ? -2e6 : -5e5; // could be a box too small to turn round in
            else if (portalDir == cd.d && target == head)
                s = 5000;
            else
                s = wanted && L <= 6 ? -500 : -4000;
            if (!anySafe)
                s += 3000;
        }
        else
        {
            // Risk is judged against its reward. A trap with nothing gained is
            // an avoidable death; a trap that pays a pearl is acceptable,
            // cheapest when a survival split can save all but 2 segments.
            int const lenAfter = L + (cd.eat ? 1 : 0);
            bool const canSave = lenAfter >= 4 && units < unitLimit;
            // No pearl compensates for losing the queen.
            if (!amQueen && !cd.ok1 && cd.dive >= diveMin)
                s += 2500.0 * cd.dive - 1500.0; // a dead end whose pearls pay for the split
            else if (QUEEN_SURV_RANK && amQueen && !anySafe && !cd.ok2)
                s -= cd.surv ? 5e5 : (QUEEN_HARD_THREAT ? QUEEN_TRAP_PEN : 1e6);
            else if (!cd.ok2)
                s -= (cd.eat && !amQueen && !(cd.dive > -1e9 && cd.dive < 0)) ? (canSave ? 2500.0 : 2500.0 + 900.0 * lenAfter)
                     : (amQueen && QUEEN_HARD_THREAT) ? QUEEN_TRAP_PEN : 1e6;
            else if (!cd.ok1)
                s -= (cd.eat && !amQueen) ? 1500.0 : amQueen ? 2e5 : 2e4;
            s += std::min(cd.area, cap) * (amQueen ? 60.0 : 15.0);
            if (QUEEN_CORRIDOR_MATE && amQueen && cd.dest >= 0 && open_sides(cd.dest) <= 2)
                for (auto const& h : heads)
                    if (h.mine && h.id != me && manhattan(h.cell, cd.dest) <= 10 && corridor_leads_to(cd.dest, h.cell, 10))
                    {
                        s -= QUEEN_EXIT_PEN; // a teammate in the same corridor: neither of us could turn round
                        break;
                    }
            if (amQueen && L >= QUEEN_LONG_LEN && cd.dest >= 0 && open_sides(cd.dest) <= 2)
                s -= QUEEN_CORRIDOR_PEN; // a long queen cannot turn round in a corridor: one blocker ahead kills her
            if (cd.eat)
                s += isLeader ? 6000 : 3000;
            if (target >= 0)
            {
                set_body(cd.nb1, L + (cd.eat ? 1 : 0));
                plan_bfs(cd.dest, distTmp);
                int dt = dist_to(distTmp, target);
                if (portalDir >= 0 && target != head && cd.dest == target)
                    dt = 0;
                cd.dt = dt;
                if (campWait)
                    s -= cd.dest == target ? 50000.0 : 150.0 * std::max(0, std::min(dt, 200) - 1);
                else
                    s -= 150.0 * std::min(dt, 200);
            }
            for (auto const& h : heads)
            {
                int const reach = (!h.mine && isLeader) ? enemy_reach(h) : 1;
                if (manhattan(h.cell, cd.dest) > reach)
                    continue;
                if (!h.mine && amQueen && QUEEN_THREAT_MAP)
                    continue; // the threat map below covers it
                if (!h.mine)
                    s -= amQueen ? 3e6
                         : isLeader ? (L >= h.len + 2 ? 200000 : 500000)
                         : (SURV_ECON && survivalMode) ? SURV_THREAT_PEN
                                    : (mode == MODE_COMBAT && role == ROLE_COMBAT ? 500 : THREAT_PEN);
                else
                    s -= isLeader || amQueen ? 4000 : TEAM_ADJ_PEN;
            }
            if (amQueen && QUEEN_THREAT_MAP)
                s -= queen_threat_pen(cd.dest);
            int esc2 = -1; // P3c: two-turn-safe cells she can reach next turn (-1: not checked)
            if (QUEEN_LOOK2 && amQueen && QUEEN_THREAT_MAP && look2Active && cd.dest >= 0 && !cd.nb1.empty())
            {
                esc2 = look2_safe_cells(cd.dest, cd.nb1, L + (cd.eat ? 1 : 0));
                s += esc2 == 0 ? -LOOK2_PEN : LOOK2_CELL_BONUS * esc2;
            }
            if (QUEEN_HOME && amQueen && cd.dest >= 0 && cd.dest == nb(head, cd.d) && pocket_ahead(head, cd.dest))
                s -= QUEEN_TRAP_PEN; // a dead end: no pearl in it is worth the queen
            else if (TOPO_TRAP && amQueen && cd.dest >= 0 && topo_trap(head, cd.d, L + (cd.eat ? 1 : 0)))
                s -= QUEEN_TRAP_PEN; // TOPO_TRAP: a dead-end branch of any shape that cannot hold her
            // v2.14 pursuit: enemy heads move too. A cell with one way on is a
            // coffin once one comes; every way on within their next two steps
            // is no better. Near them, open water beats hugging a wall.
            if (PURSUIT_SAFE && amQueen && cd.dest >= 0 && cd.d != bunkerDir)
            {
                int nearE = INF, nearAny = INF;
                for (auto const& h : heads)
                    if (h.id != me)
                    {
                        nearAny = std::min(nearAny, manhattan(h.cell, cd.dest));
                        if (!h.mine)
                            nearE = std::min(nearE, manhattan(h.cell, cd.dest));
                    }
                // Teammates move too: one stepping into her last way on boxes her
                // in, and in a 1-wide corridor one coming the other way does.
                if (open_sides(cd.dest) <= 2 && open_sides(head) >= 3)
                    s -= (nearAny <= 6 ? CORR_ENTER_PEN : CORR_ENTER_PEN / 5);
                {
                    int ex = 0, exHit = 0;
                    int const nl = L + (cd.eat ? 1 : 0);
                    for (int d2 = 0; d2 < 4; d2++)
                    {
                        int const n = cross(cd.dest, d2);
                        if (n < 0 || n == head)
                            continue;
                        bool blocked = vis[n] && occId[n] >= 0 && occId[n] != me;
                        for (int i = 0; i + 1 < nl && i < static_cast<int>(cd.nb1.size()); i++)
                            if (cd.nb1[i] == n)
                                blocked = true;
                        if (blocked)
                            continue;
                        ex++;
                        for (auto const& h : heads)
                            if (!h.mine && manhattan(h.cell, n) <= 2 * std::max(1, (std::max(h.len, h.estLen) + 3) / 4))
                            {
                                exHit++;
                                break;
                            }
                    }
                    if (LOOK2_SKIP_CORNER && esc2 > 0)
                        ; // P3c: a two-turn-safe way on exists - the corner rules are only proxies for that
                    else if (ex <= 1)
                        s -= (nearE <= PURSUIT_DIST || nearAny <= 3) ? CORNER_PEN : CORNER_PEN / 5;
                    else if (exHit >= ex && nearE <= PURSUIT_DIST)
                        s -= ALL_EXITS_PEN;
                    if (nearE <= 3)
                        s += QUEEN_MOBILITY * ex;
                }
            }
            if (cd.noLoop && cd.d != bunkerDir)
                s -= queenStarving ? 50000 : QUEEN_LOOP_PEN;
            if (amQueen && queenSealed && cd.eat && L + 1 > sealedCells - 1)
                s -= 2e6; // eating would fill the sealed pocket: no free cell left to move into
            if (amQueen && !vis[cd.dest])
                s -= queenStarving ? 1e5 : 5e5; // a portal whose far side we cannot see: only as a last resort
            if (amQueen && QUEEN_AVOID_PORTAL && !queenStarving)
            {
                std::uint8_t const et = edgeType[edge_code(head, cd.d)];
                if (et != E_OPEN && et != E_KELP && et != E_UNK)
                    s -= 1e6; // crossing a portal: never by choice
                for (int k = 0; k < 4; k++)
                {
                    std::uint8_t const t2 = edgeType[edge_code(cd.dest, k)];
                    if (t2 != E_OPEN && t2 != E_KELP && t2 != E_UNK)
                        s -= 150000; // next to a portal mouth
                }
            }
            if (amQueen && QUEEN_AVOID_UNKNOWN && !queenStarving)
            {
                for (int k = 0; k < 4; k++)
                    if (edgeType[edge_code(cd.dest, k)] == E_UNK)
                        s -= 30000;
                if (seenRound[cd.dest] < 0)
                    s -= 200000;
            }
            if (!amQueen && !vis[cd.dest])
            {
                // Blind portal exit: we cannot see who stands there. Never risk
                // landing on the queen, who moves first and may be right there.
                s -= 3000;
                auto const q = fleet.find(queenId);
                if (q != fleet.end() && R - q->second.round <= 3 && manhattan(q->second.cell, cd.dest) <= 2 + (R - q->second.round))
                    s -= 2e6;
            }
            if (amQueen) // stay clear of where the fleet saw enemies recently, even out of view
                for (auto const& e : enemyReps)
                {
                    if (R - e.round > 8)
                        continue;
                    int const dd = manhattan(e.cell, cd.dest);
                    if (dd <= 7)
                        s -= 4000.0 * (8 - dd);
                }
            if (amQueen) // keep the queen well away from enemy heads, not just out of reach
                for (auto const& h : heads)
                    if (!h.mine)
                    {
                        int const dd = manhattan(h.cell, cd.dest);
                        if (dd <= 5)
                            s -= 25000.0 * (6 - dd);
                    }
            if (isLeader) // an enemy that splits sends a new head out of its tail
                for (int tc : enemyTails)
                    if (manhattan(tc, cd.dest) <= 1)
                        s -= amQueen ? 400000 : 100000;
            for (auto const& dg : dangers)
                if (R - dg.round <= 30 && manhattan(dg.cell, cd.dest) <= 3)
                    s -= isLeader ? 3000 : 300;
            // Confinement: when we are safe, prefer moves that squeeze a nearby
            // enemy's free space (pin it against kelp and bodies).
            if (cd.ok2 && cd.area >= 2 * L + 4)
                for (auto const& h : heads)
                {
                    if (h.mine || manhattan(h.cell, cd.dest) > 4)
                        continue;
                    int const ecap = 2 * h.len + 8;
                    set_body(cd.nb1, L + (cd.eat ? 1 : 0));
                    ignoreContested = true;
                    FloodRes const ef = flood(h.cell, -1, ecap);
                    ignoreContested = false;
                    int const earea = ef.area + (ef.frontier ? ecap : 0);
                    double const mult = h.id == enemyQueenId ? 3.0 : 1.0;
                    if (earea <= h.len + 1)
                        s += 3500 * mult;
                    else
                        s += mult * 1500.0 * std::max(0.0, 1.0 - static_cast<double>(earea) / ecap);
                }
            if (!isLeader && std::string(why) != "feed")
                for (int tc : teamCells)
                    s -= 150.0 / (1 + manhattan(tc, cd.dest));
            if (PORTAL_ROW && cd.dest >= 0 && cd.dest != nb(head, cd.d))
            {
                // Through a portal: a teammate whose head is at (or next to) the
                // far end may be stepping the other way this very round, and
                // the two heads would meet. The lower id goes first.
                auto clash = [&](int id, int cell) {
                    int const dd = manhattan(cell, cd.dest);
                    return dd == 0 || (dd == 1 && id < me);
                };
                bool hit = false;
                for (auto const& h : heads)
                    if (h.mine && h.id != me && clash(h.id, h.cell))
                        hit = true;
                for (auto const& [id, st] : fleet)
                    if (id != me && R - st.round <= 1 && clash(id, st.cell))
                        hit = true;
                if (hit)
                    s -= PORTAL_ROW_PEN;
            }
            if (eqHead >= 0 && !cd.nb1.empty())
            {
                int const after = queen_exits(eqHead, cd.nb1);
                s += BOX_BONUS * (eqExitsNow - after);
                if (after == 0)
                    s += BOX_BONUS; // no way out: she must split or die
            }
            if (bunkerFresh && cd.dest >= 0 && std::find(qBox.begin(), qBox.end(), cd.dest) != qBox.end())
                s -= QUEEN_EXIT_PEN; // her bunker box is hers alone
            if (qCorr >= 0 && cd.dest >= 0 && open_sides(cd.dest) <= 2 && manhattan(cd.dest, qCorr) <= manhattan(head, qCorr) &&
                corridor_leads_to(cd.dest, qCorr, 12))
                s -= QUEEN_EXIT_PEN; // never walk into her corridor towards her: she cannot turn round there
            bool const onPost = bunkerFresh && std::string(why) == "post"; // her way out is her box loop, which nobody enters
            if (QUEEN_EXIT_GUARD && !amQueen && !onPost && !cd.nb1.empty()) // feeders too: they can give their pearls from 4 cells away
                for (auto const& h : heads)
                    if (h.mine && h.id == queenId && manhattan(h.cell, cd.dest) <= 2)
                    {
                        std::vector<int> nowBody(body.begin(), body.begin() + std::min<int>(body.size(), L));
                        std::vector<int> after(cd.nb1.begin(), cd.nb1.begin() + std::min<int>(cd.nb1.size(), L + (cd.eat ? 1 : 0)));
                        int const before = queen_exits(h.cell, nowBody), now = queen_exits(h.cell, after);
                        if (now < before && now < 2)
                            s -= QUEEN_EXIT_PEN; // never take one of the queen's last ways out
                    }
            if (!amQueen && !onPost && (std::string(why) != "feed" || (consolidate && !(HARVEST && R >= harvest_start()))))
                for (auto const& h : heads)
                    if (h.mine && h.id == queenId)
                    {
                        // Do not box the queen in: her own body is mostly out of
                        // our view, so keep clear of her head instead.
                        int const dq = manhattan(h.cell, cd.dest);
                        if (dq <= 2)
                            s -= QUEEN_SPACE_PEN;
                        else if (dq == 3)
                            s -= QUEEN_SPACE_PEN / 3.5;
                    }
            if (cd.d == facing)
                s += 3;
        }
        cd.score = s;
        if (s > bestScore)
        {
            bestScore = s;
            bestD = cd.d;
        }
    }
    set_body(body, L);
    if (bestD < 0)
        bestD = facing;
#ifdef QDEBUG
    if (amQueen)
    {
        int nearE = INF;
        for (auto const& h : heads)
            if (!h.mine)
                nearE = std::min(nearE, manhattan(h.cell, head));
        if (nearE <= 8)
        {
            std::string dbg = std::string("QDBG ") + why + " best" + DCH[bestD];
            for (auto const& cd : cands)
            {
                dbg += std::string(" ") + DCH[cd.d] + (cd.legal ? "" : "x");
                if (!cd.legal || cd.dest < 0)
                    continue;
                dbg += "@" + std::to_string(cd.dest % W) + "," + std::to_string(cd.dest / W) + "s" + std::to_string(static_cast<long long>(cd.score)) +
                       "t" + (qThreat[cd.dest] >= INF ? std::string("-") : std::to_string(qThreat[cd.dest])) + "o" + std::to_string(cd.ok1) +
                       std::to_string(cd.ok2) + "a" + std::to_string(cd.area);
            }
            for (auto const& h : heads)
                if (!h.mine)
                    dbg += " E" + std::to_string(h.id) + "@" + std::to_string(h.cell % W) + "," + std::to_string(h.cell / W) + "l" +
                           std::to_string(h.len) + "/" + std::to_string(h.estLen);
            ct.output_log(dbg);
        }
    }
#endif
    // The way to the pearl only leads into a dead end that does not pay:
    // stop circling outside and let the matching pick another pearl.
    if (pi >= 0 && target == pearls[pi].cell && distHead[target] < INF)
    {
        bool closer = false, closerOk = false;
        for (auto const& cd : cands)
            if (cd.legal && !cd.blind && cd.dt < distHead[target])
            {
                closer = true;
                closerOk |= cd.ok1 || cd.dive >= diveMin;
            }
        if (closer && !closerOk)
            skipPearl[target] = R + SKIP_PEARL_ROUNDS;
    }

    // Free steps: keep going along the chosen direction while it pays (more
    // pearls, more progress) and the turn still ends somewhere safe.
    Cand const& c1 = cands[bestD];
    if (DEADEND_STRICT && !amQueen && c1.legal && !c1.blind && !c1.ok1 && c1.dive >= std::max(diveMin, 1.0))
        miningTrip = true; // going in for pearls that pay for the stub: no split until the bottom
    // A paid step wins a pearl race an enemy head would otherwise win
    // (theirs do this constantly); never for the queen.
    bool paidRace = false;
    if (PAID_PEARL_RACE && !amQueen && !survivalMode && L >= 3 && std::string(why) == "pearl" && target >= 0 && distHead[target] == 2)
        for (auto const& h : heads)
            if (!h.mine && manhattan(h.cell, target) <= 2)
                paidRace = true;
    if ((freeN >= 2 || paidRace) && c1.legal && !c1.blind && c1.ok2 && target >= 0 && portalDir < 0 && !c1.nb1.empty() &&
        !(amQueen && SURVIVAL && survivalMode) && !campWait)
    {
        stamp++; // distances to the goal ignore our own body
        plan_bfs(target, distGoal);
        set_body(body, L);
        int const g0 = distGoal[head];
        bool const useGoal = g0 < INF;
        int const nl = L + (c1.eat ? 1 : 0);
        int const cap1 = std::min(N, 3 * nl + 24);
        double const v1 = (c1.eat ? 3000.0 : 0.0) + std::min(c1.area, cap1) * 15.0 - end_threat(c1.dest) +
                          (useGoal && distGoal[c1.dest] < INF ? 150.0 * (g0 - distGoal[c1.dest]) : 0.0);
        SprintPlan sp;
        std::vector<int> dirs{bestD}, eatenCells;
        if (c1.eat)
            eatenCells.push_back(c1.dest);
        sprint_dfs(c1.nb1, nl, 1, paidRace ? std::max(freeN, 2) : freeN, freeN, c1.eat ? 1 : 0, dirs, eatenCells, useGoal, g0, sp);
        set_body(body, L);
        bool sprintOk = sp.kill || (sp.value > v1 + 300 && sp.dirs.size() >= 2);
        if (paidRace && !sp.kill && sp.dirs.size() >= 2)
        {
            // Only when the sprint really lands on the raced pearl (the paid
            // step costs more than one pearl is worth in the plan's own score).
            int c = head;
            bool hits = false;
            for (int d : sp.dirs)
            {
                c = c >= 0 ? cross(c, d) : -1;
                hits |= c == target;
            }
            sprintOk = sprintOk || (hits && sp.value > v1 - 1500);
        }
        if (sprintOk && amQueen && !sp.kill)
        {
            int el = 0;
            std::vector<int> const endBody = simulate_steps(body, L, sp.dirs, el);
            sprintOk = !endBody.empty() && queen_can_survive(endBody, el) &&
                       !(QUEEN_LOOP_SAFE && !loop_reachable(endBody, el));
            if (sprintOk && PURSUIT_SAFE)
            {
                int const e = endBody[0];
                int nearE = INF;
                for (auto const& h : heads)
                    if (!h.mine)
                        nearE = std::min(nearE, manhattan(h.cell, e));
                if (nearE <= PURSUIT_DIST + 1)
                {
                    int ex = 0;
                    for (int d2 = 0; d2 < 4; d2++)
                    {
                        int const n = cross(e, d2);
                        if (n < 0)
                            continue;
                        bool blocked = vis[n] && occId[n] >= 0 && occId[n] != me;
                        for (int i = 0; i + 1 < el && i < static_cast<int>(endBody.size()); i++)
                            if (endBody[i] == n)
                                blocked = true;
                        ex += !blocked;
                    }
                    if (ex <= 1)
                        sprintOk = false; // a corner with an enemy near: one step and she is boxed
                }
            }
            set_body(body, L);
        }
        if (sprintOk)
            return act_moves(ct, sp.dirs, sp.kill ? "regicide" : why);
    }
    act_move(ct, bestD, why);
}

// ---------------------------------------------------------------------------
// Sonar output (section 4): four messages a turn, N/E/S/W.
// ---------------------------------------------------------------------------
// Direction of a sonar ray that runs straight to our queen (same row or
// column, the shorter way round), or -1.
int queen_sonar_dir()
{
    if (me == queenId || ourQueenDead || body.empty())
        return -1;
    auto const q = fleet.find(queenId);
    if (q == fleet.end() || R - q->second.round > 3)
        return -1;
    int const h = body[0], qc = q->second.cell;
    int const hx = h % W, hy = h / W, qx = qc % W, qy = qc / W;
    if (hy == qy && hx != qx)
        return ((qx - hx + W) % W) <= ((hx - qx + W) % W) ? 1 : 3;
    if (hx == qx && hy != qy)
        return ((qy - hy + H) % H) <= ((hy - qy + H) % H) ? 2 : 0;
    return -1;
}

void send_sonars(Controller& ct)
{
    std::vector<std::uint64_t> out;
    out.push_back(msg_status());
    if (QUEEN_POS_RELAY && amQueen)
        out.push_back(out[0]); // two directions a turn: every teammate in line hears where she is
    if (claimTarget >= 0)
        out.push_back(msg_claim(claimTarget, claimEta));

    std::sort(facts.begin(), facts.end(), [](Fact const& a, Fact const& b) { return a.prio > b.prio; });
    for (auto& f : facts)
    {
        if (out.size() >= 4)
            break;
        if (f.prio < 30 && !pearlQ.empty() && out.size() == 3)
            break; // leave room for pearl news
        out.push_back(f.msg);
        f.sends--;
    }
    std::erase_if(facts, [](Fact const& f) { return f.sends <= 0; });

    if (out.size() < 4 && !pearlQ.empty())
    {
        std::sort(pearlQ.begin(), pearlQ.end(), [](PearlEntry const& a, PearlEntry const& b) { return a.prio > b.prio; });
        BitW b;
        int const take = std::min<int>(2, static_cast<int>(pearlQ.size()));
        b.put(take - 1, 1);
        for (int i = 0; i < take; i++)
        {
            b.put(pearlQ[i].kind, 2);
            b.put(pearlQ[i].cell, 12);
            b.put(pearlQ[i].round, 9);
        }
        pearlQ.erase(pearlQ.begin(), pearlQ.begin() + take);
        out.push_back(pack(T_PEARL, b.v));
    }

    // Gossip what we already know, so late arrivals catch up.
    for (int attempt = 0; attempt < 12 && out.size() < 4; attempt++)
    {
        int const pick = static_cast<int>(rng() % 3);
        if (pick == 0 && !kelpList.empty())
        {
            BitW b;
            int const n = std::min<int>(3, static_cast<int>(kelpList.size()));
            b.put(n, 2);
            for (int i = 0; i < n; i++)
                b.put(kelpList[rng() % kelpList.size()], 13);
            out.push_back(pack(T_KELP, b.v));
        }
        else if (pick == 1 && !portalPairs.empty())
        {
            auto const& pp = portalPairs[rng() % portalPairs.size()];
            out.push_back(msg_portal(pp.first, pp.second));
        }
        else if (pick == 2)
        {
            // a known upcoming or present pearl
            int found = -1;
            for (int tries = 0; tries < 24 && found < 0; tries++)
            {
                int const c = static_cast<int>(rng() % static_cast<unsigned>(N));
                if (pearlState[c] == 1 || (nextSpawn[c] >= R && !nextEst[c] && nextSpawn[c] - R <= 30))
                    found = c;
            }
            if (found < 0)
                continue;
            BitW b;
            b.put(0, 1);
            bool const present = pearlState[found] == 1;
            b.put(present ? 1 : 0, 2);
            b.put(found, 12);
            b.put(present ? pearlRound[found] : nextSpawn[found], 9);
            out.push_back(pack(T_PEARL, b.v));
        }
    }

    int const offset = (R + me) & 3;
    std::array<int, 4> dirOf{};
    for (int i = 0; i < 4; i++)
        dirOf[i] = (i + offset) & 3;
    // Aim the most urgent enemy news at the queen when she is on our row or
    // column (a ping stops at the first dragon it meets, so nearer is better).
    int const qd = AIM_REPORTS ? queen_sonar_dir() : -1;
    if (qd >= 0)
    {
        int best = -1;
        for (int i = 0; i < static_cast<int>(out.size()); i++)
        {
            int type;
            std::uint64_t payload;
            if (unpack(out[i], type, payload) && (type == T_ENEMY || type == T_DANGER))
            {
                best = i;
                break; // facts are already in priority order
            }
        }
        if (best >= 0)
            for (int i = 0; i < 4; i++)
                if (dirOf[i] == qd)
                {
                    if (i >= static_cast<int>(out.size()))
                        out.resize(i + 1, out[0]);
                    std::swap(out[i], out[best]);
                    break;
                }
    }
    // DECOYS: a ray whose first dragon in view is an enemy cannot reach a teammate; it carries one of their own
    // stale messages instead (the stalest within DECOY_MIN_AGE..DECOY_MAX_AGE rounds).
    if (DECOYS && !foreignMsgs.empty() && !body.empty())
        for (int i = 0; i < 4; i++)
        {
            int c = body[0];
            bool enemyFirst = false;
            for (int k = 0; k < 3; k++)
            {
                c = cross(c, dirOf[i]);
                if (c < 0 || !vis[c])
                    break; // kelp, unknown or out of view: we cannot tell who it reaches
                if (occId[c] >= 0)
                {
                    enemyFirst = occId[c] != me && !occMine[c];
                    break;
                }
            }
            if (!enemyFirst)
                continue;
            int pick = -1;
            for (int j = 0; j < static_cast<int>(foreignMsgs.size()); j++)
            {
                int const age = R - foreignMsgs[j].round;
                if (age >= DECOY_MIN_AGE && age <= DECOY_MAX_AGE && (pick < 0 || foreignMsgs[j].round < foreignMsgs[pick].round))
                    pick = j;
            }
            if (pick < 0)
                break;
            if (i >= static_cast<int>(out.size()))
                out.resize(i + 1, 0);
            out[i] = foreignMsgs[pick].msg;
            decoysSent++;
        }
    for (int i = 0; i < static_cast<int>(out.size()) && i < 4; i++)
        if (out[i] != 0)
            ct.send_sonar(dir_of(dirOf[i]), out[i]);
}

// ---------------------------------------------------------------------------
// Fallback: any move that is not immediately fatal.
// ---------------------------------------------------------------------------
void fallback_move(Controller& ct)
{
    Position const here = ct.get_position();
    Tile const* t = ct.get_tile(here);
    for (int d = 0; d < 4; d++)
    {
        if (t && !t->get_edge(dir_of(d)).is_passable())
            continue;
        Tile const* ahead = ct.get_tile(here.add_dir(dir_of(d)));
        if (ahead && ahead->get_dragon())
            continue;
        ct.make_move(dir_of(d));
        return;
    }
    // Boxed in: a survival split keeps the head (the child takes the tail).
    if (ct.can_split(2) && ct.get_length() >= 4)
    {
        ct.do_split(2);
        return;
    }
    ct.make_move(ct.get_dir());
}

void init_world(Controller& ct, Game& game)
{
    auto const [w, h] = game.get_map_size();
    W = w;
    H = h;
    N = W * H;
    me = ct.get_id();
    myTeam = ct.get_team().value;
    unitLimit = game.get_unit_limit();
    rng.seed(static_cast<std::uint32_t>(me) * 7919u + 17u);
    // Map files list team A's dragons first, so A usually owns id 0; vision and
    // sonar correct this as soon as either queen is seen or heard.
    queenId = me <= 1 ? me : (myTeam == Team::A ? 0 : 1);
    enemyQueenId = 1 - queenId;
    amQueen = me == queenId;
    role = role_for(me);

    edgeType.assign(2 * N, E_UNK);
    obsEdge.assign(2 * N, E_UNK);
    edgePid.assign(2 * N, -1);
    edgePartner.assign(2 * N, -1);
    seenRound.assign(N, -1);
    obsNext.assign(N, -1);
    nextSpawn.assign(N, -1);
    nextEst.assign(N, 0);
    pearlState.assign(N, 0);
    dropRound.assign(N, -1);
    pearlRound.assign(N, -1);
    gapLo.assign(N, 0);
    gapHi.assign(N, 0);
    vis.assign(N, 0);
    occId.assign(N, -1);
    occMine.assign(N, 0);
    occHead.assign(N, 0);
    occDir.assign(N, 0);
    otherRel.assign(N, 0);
    contested.assign(N, 0);
    ownStamp.assign(N, 0);
    ownRel.assign(N, 0);
    wallStamp.assign(N, 0);
    visStamp.assign(N, 0);
    distHead.assign(N, INF);
    distTmp.assign(N, INF);
    distGoal.assign(N, INF);
    pathStamp.assign(N, 0);
    pathTime.assign(N, 0);
    skipPearl.assign(N, 0);
    planSteps.assign(N, 0);
    qThreat.assign(N, INF);
    qThreatVis.assign(N, INF);
    qThreatCnt.assign(N, 0);
    qThreat2.assign(N, INF);
    l2Stamp.assign(N, 0);
    l2Dist.assign(N, 0);
    qMargin.assign(N, INF);
    qDist.assign(N, INF);
    thStamp.assign(N, 0);
    thDist.assign(N, 0);
    thQ.reserve(N);
    bfsQ.reserve(N);
    bfsT.reserve(N);
}

} // namespace

int main()
{
    auto [ct, game] = unswbc::init();
    init_world(ct, game);

    while (unswbc::update(ct, game))
    {
        bool acted = false;
        try
        {
            R = game.get_round_num();
            L = ct.get_length();
            units = ct.get_unit_count();

            read_vision(ct);
            update_body(ct);
            read_sonar(ct);
            update_unobserved();
            resolve_symmetry();
            prune_knowledge();
            detect_teammate_deaths();
            record_enemies();
            fleet[me] = {L, body[0], R, isLeader};
            compute_fleet_state();

            decide(ct);
            acted = true;
            send_sonars(ct);

            static char const* const roleName[] = {"col", "exp", "prt", "cmb"};
            static char const* const modeName[] = {"nrm", "farm", "keep", "war"};
            std::string ind = std::string(amQueen ? "QUEEN " : isLeader ? "LEAD " : "") + roleName[role] + " " + modeName[mode] + " " + note +
                              " L" + std::to_string(L);
            if (symKnown >= 0)
                ind += " s" + std::to_string(symKnown);
            if (isScreen || isGuard)
                ind += isScreen ? " SCR" : " GRD";
            if (LOG_PHASE)
                ind += " P" + std::to_string(pressureMax) + (switched ? "*" : "");
            ct.set_indicator_string(ind);
        }
        catch (...)
        {
            if (!acted)
                fallback_move(ct);
        }
        unswbc::end_turn();
    }
}
