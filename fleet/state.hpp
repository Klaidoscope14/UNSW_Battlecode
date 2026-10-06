// STATE - this dragon's strategy state: roles, modes, phase flags, queen identity and status,
// commitments. (Architecture_v2: becomes the Situation struct in step 2.)
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// Strategy state
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
bool openMap = false;     // big map with little kelp: long dragons do not trap themselves
bool consolidate = false; // big open map, late: length over numbers
bool isScreen = false; // guard posted out toward the enemy to see attackers early
bool isHeir = false; // enemy queen dead: the dragon that grows instead of our queen
bool isWall = false;  // P8: in the moving wall (joins the packs)
bool wall_now();
bool wall_slot(int id);
bool isGiant = false;
bool roleLeader = false; // F1: isLeader came from the giant / heir role (cleared when the role ends)
std::vector<int> chaseCnt, chaseLast; // F4: rounds each pearl was chased, last round chased
int dryStart = -1;                    // F4: first round of the current run of "pearl" without eating
int campTile = -1, campArriveR = -1;
bool haveMajority = false, openPlan = false; // OPEN_PLAN
struct ZoneRep
{
    int cell, tiles, round;
};
std::vector<ZoneRep> zoneReps; // pearl-rich zones heard over sonar
int est_enemy_units();  // F6: the tile we camp by, and since when we are next to it // P5: our longest non-queen dragon from GIANT_ROUND (insurance for the longest-dragon tiebreak)
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

} // namespace
