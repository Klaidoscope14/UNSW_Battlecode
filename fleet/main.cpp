// Fleet bot: numbers and information first, then size and control.
//
// Every dragon runs its own copy of this program and shares nothing but sonar,
// so the "fleet" behaviour is emergent: each dragon keeps a persistent model of
// the map, tells teammates what it learns, and makes decisions that stay
// consistent with what the others are likely to decide from the same facts.
// Dragons 0 and 1 are the queens; after round 500 the longer living queen wins.
//
// Layout (Architecture_v2.txt): one translation unit, the modules below are
// included in dependency order.
//   config.hpp     every switch, grouped by module
//   core.hpp       grid geometry, sizes, rng, decision trace
//   world.hpp      L0 what we know (edges, portals, symmetry, pearls, dragons, reports)
//   state.hpp      this dragon's strategy state and phase flags
//   comms.hpp      L7 wire format and outboxes
//   perceive.hpp   L0 reading vision / body / sonar, learning, death detection
//   graph.hpp      L1 occupancy over time, flood fill, BFS, survival search, dives
//   fields.hpp     L1 per-turn maps: distances, richness, Voronoi, enemy reach
//   situation.hpp  L2 phases, roles by id, leaders / giant / heir, tiebreak guess
//   topology.hpp   L1 corridors, pockets, dead ends, free loops
//   commander.hpp  L3 team assignment: pearls, hunter / wall slots, packs, prey
//   intent.hpp     L5 goal choosers (economy, queen rest / flee / bunker, screens)
//   tactics.hpp    L4 move candidates, threat terms, lookahead, strike paths
//   act.hpp        L6 split gates, moves / sprints / splits, sprint planner
//   decide.hpp     the per-turn decision
//   speak.hpp      L7 the four sonar rays
// Per turn: perceive -> fleet state -> decide -> speak (see main() below).

#include "helper.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace unswbc;

#include "config.hpp"

#include "core.hpp"
#include "world.hpp"
#include "state.hpp"
#include "comms.hpp"
#include "perceive.hpp"
#include "graph.hpp"
#include "fields.hpp"
#include "situation.hpp"
#include "topology.hpp"
#include "commander.hpp"
#include "intent.hpp"
#include "tactics.hpp"
#include "act.hpp"
#include "decide.hpp"
#include "speak.hpp"

namespace {

// Fallback: any move that is not immediately fatal.
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
    if (char const* dir = std::getenv("FLEET_TRACE"))
    {
        std::string const path = std::string(dir) + "/" + (myTeam == Team::A ? "A" : "B") + std::to_string(me) + ".txt";
        traceF = std::fopen(path.c_str(), "w");
    }

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
            if (isWall)
                ind += " WAL";
            if (LOG_PHASE)
                ind += " P" + std::to_string(pressureMax) + (switched ? "*" : "");
            ct.set_indicator_string(ind);
        }
        catch (...)
        {
            if (!acted)
                fallback_move(ct);
        }
        if (traceF)
        {
            std::fprintf(traceF, "%d %d %d %d %s | %s\n", R, me, L, body.empty() ? -1 : body[0], traceAct.c_str(), note.c_str());
            std::fflush(traceF);
            traceAct = "fallback";
        }
        unswbc::end_turn();
    }
}
