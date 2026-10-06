// STATE - this dragon's strategy state:
//   Situation sit  team-level facts (phase, strength, map class, queens), computed in compute_fleet_state
//   Duty my        this dragon's roles, decided in compute_fleet_state
//   then identity, queen status, and the commitments a dragon carries from one turn to the next.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

enum Role { ROLE_COLLECTOR, ROLE_EXPLORER, ROLE_PORTAL, ROLE_COMBAT };
enum Mode { MODE_NORMAL, MODE_FARM, MODE_PRESERVE, MODE_COMBAT };

// L2: where we stand. Clock, strength, map and queens are team-level; local threat is this dragon's.
struct Situation
{
    // Clock / phase
    bool expansion = true;    // numbers first, until the fleet first reaches its expansion target
    bool openPlan = false;    // OPEN_PLAN: multiply until we have the majority
    bool majority = false;    // OPEN_PLAN majority (with hysteresis)
    bool switched = false;    // DYN_ALLOC: numbers -> length switch has happened (synced by T_PHASE)
    int switchRound = SW_HI;  // DYN_ALLOC: when it happens, from the enemy pressure
    int pressureMax = 0;      // DYN_ALLOC: most enemies anyone has seen
    bool heardPressure = false;
    int firstR = -1;          // first round this dragon played
    bool transitioned = false; // leader phase (section 12 table)
    int transitionRound = 300;
    bool consolidate = false; // late: length over numbers
    bool lateSafe = false;    // v2.12: last rounds - the queen keeps to safe ground
    bool queenSplits = true;  // the queen splits rather than grows (see QUEEN_GROW_ROUND)
    bool fleetEstablished = false; // P2: the fleet once had ADAPT_FLEET dragons
    bool queenOpening = false;     // P2: the queen splits and roams as before the home rule
    // Strength
    bool survival = false;    // v2.12: tiny fleet, heavily outnumbered or the last rounds
    bool outnumbered = false;
    bool majorityHunt = false; // smaller map with a clear majority: every spare dragon hunts
    int enemyEst = 0;          // v2.14: enemy count for the trade gates (scaled by our coverage)
    int distinctEnemies = 0;   // enemy ids heard of in the last 60 rounds
    // Map
    bool openMap = false;      // big map with little kelp: long dragons do not trap themselves
    // Queens
    bool ourQueenDead = false, enemyQueenDead = false;
    int lastEqSeenR = -1000;   // last round their queen was seen or reported
    // Local (this dragon)
    int localThreat = 0;
    int echoEnemy = 0;
    Mode mode = MODE_NORMAL;
};
Situation sit;

// L3: this dragon's roles.
struct Duty
{
    Role role = ROLE_COLLECTOR; // id-hash temperament (collector / explorer / portal / combat)
    bool leader = false;     // the queen, elected leaders, the giant and the heir
    bool roleLeader = false; // F1: leader only through the giant / heir role (cleared when the role ends)
    bool hunterSlot = false; // in the hunter group (scouts portals before the hunt starts)
    bool hunter = false;
    bool guard = false;
    bool screen = false;     // guard posted out toward the enemy to see attackers early
    bool defender = false;   // holds our half (medium length, camps near the middle)
    bool wall = false;       // P8: in the moving wall (joins the packs)
    bool giant = false;      // P5: our longest non-queen dragon grows (longest-dragon tiebreak)
    bool heir = false;       // enemy queen dead: the dragon that grows instead of our queen
};
Duty my;

bool wall_now();
bool wall_slot(int id);
int est_enemy_units();

// Identity: ids 0 and 1 are the queens; which one is ours is learned.
int queenId = 0, enemyQueenId = 1;
bool amQueen = false;
bool queenSeen = false; // queen identity confirmed by vision (team is shown there)
int ourQueenSpawn = -1; // where our queen started (the enemy queen started at its mirror image)

// Reports kept beside the world model
struct ZoneRep
{
    int cell, tiles, round;
};
std::vector<ZoneRep> zoneReps; // pearl-rich zones heard over sonar

// The queens
int qBunkerTl = -1, qBunkerR = -1, inferTl = -1, inferR = -1; // the queen's bunker box as she last announced it
bool sealedChecked = false, queenSealed = false;
int sealedCells = 0; // size of the queen's sealed starting pocket
bool queenStarvingG = false; // v2.14: the queen's starving flag, for pearl gathering
std::unordered_map<int, std::pair<int, int>> escortSeen; // enemy id -> (last round within 2 of our queen, rounds in a row)
int restCell = -1;          // v2.14: the queen's resting spot (open water inside her leash)
int queenLeash = QUEEN_LEASH; // P2: her leash this turn
int lastThreatR = -1000;      // last round an enemy head was near her
int prevEnemyQueenCell = -1;
int prevEnemyQueenLen = 0;
std::vector<int> prevEnemyQueenBody, eqBodyNow; // her body cells we saw last turn / this turn

// Commitments: what this dragon carries from one turn to the next
int lastEatR = 0, lastLenSeen = 0;
std::vector<int> chaseCnt, chaseLast; // F4: rounds each pearl was chased, last round chased
int dryStart = -1;                    // F4: first round of the current run of "pearl" without eating
int campTile = -1, campArriveR = -1;  // F6: the tile we camp by, and since when we are next to it
std::vector<int> headHist;  // our head, last rounds (loop stall)
bool miningTrip = false;    // v2.15: on a dead-end trip that paid >= 1 length when we went in
int lastPearl = -1;         // the pearl this dragon went for last turn
int stallTarget = -1, stallBest = INF, stallRound = 0; // progress toward the current pearl
int exploreGoal = -1, exploreSetRound = -1000;
int claimTarget = -1, claimEta = 0;
std::string note;

} // namespace
