// L5 INTENT - goal choosers: pearls, exploring, camping, outposts, zones, wall line, portals,
// the queen's rest / flee / bunker, guard screens.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {

// The goal a dragon is heading for this turn (shown in the indicator and the trace).
enum Intent
{
    I_EXPLORE,
    I_PEARL,
    I_LATE_SAFE,
    I_EVADE,
    I_FLEE,
    I_HUNT,
    I_HUNT_QUEEN,
    I_FEED,
    I_HUNTER,
    I_PACK,
    I_LURK,
    I_REGROUP,
    I_WALL,
    I_INTERCEPT,
    I_POST,
    I_SCREEN,
    I_GUARD,
    I_GUARD_LINE,
    I_RESCUE,
    I_BUNKER,
    I_REST,
    I_HOME,
    I_PORTAL,
    I_PUSH,
    I_DEFEND,
    I_CAMP,
    I_ZONE,
    I_OUTPOST,
    I_CORK,
};
// 6b: the pearl leash of each role - a role's duty replaces the pearl goal unless the pearl is at most this close
// (one table instead of literals spread over the target steps; same values as before).
struct PearlLeash
{
    int hunt = 4, majHunt = 2, feed = 6, pack = 2, wall = 2, screen = 3, guard = 4, defender = 10;
    int queenAfterTheirs = 3; // our queen once theirs is dead: no pearl trips beyond this
    int zone = ZONE_OVERRIDE, scoutMargin = SCOUT_PEARL_MARGIN;
};
constexpr PearlLeash kLeash{};

char const* intent_name(Intent i)
{
    static char const* const names[] = {"explore", "pearl", "late-safe", "evade", "flee", "hunt", "hunt-queen", "feed", "hunter", "pack", "lurk", "regroup", "wall", "intercept", "post", "screen", "guard", "guard-line", "rescue", "bunker", "rest", "home", "portal", "push", "defend", "camp", "zone", "outpost", "cork"};
    return names[i];
}

std::vector<PearlCand> gather_pearls()
{
    compute_voronoi();
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
                 nextSpawn[c] - R <= (CAMP_JOB && !amQueen ? (OPEN_HOLD && R < OPEN_HOLD_END && distHead[c] <= HOLD_DIST // O3
                                                                  ? std::max(distHead[c] + CAMP_LEAD, HOLD_WAIT)
                                                                  : (distHead[c] < INF ? distHead[c] : 0) + CAMP_LEAD)
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
        // Contested: an enemy head is clearly closer (A: by path, reports included).
        if (VORONOI && static_cast<int>(vorEnemy.size()) == N)
        {
            if (vorEnemy[c] + 1 < eta)
                conf *= (AGGRO_EXPANSION && sit.expansion && !amQueen) ? 0.8 : 0.35;
        }
        else
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, c) + 1 < eta)
                    conf *= (AGGRO_EXPANSION && sit.expansion && !amQueen) ? 0.8 : 0.35;
        if (OWN_HALF && halfMargin(c) > 0)
            conf *= OWN_HALF_BONUS; // ours first: raiders feed on what we leave
        // v2.14: the queen keeps to her home ground, away from enemy heads, and
        // never goes into a pocket (a dead end is where escorts finish her).
        bool const herDrop = FEED_FIX && amQueen && eta <= FEED_EAT_DIST && pearlState[c] == 1 && qThreat[c] >= INF;
        if (amQueen && QUEEN_HOME && !queenStarvingG && !herDrop)
        {
            if (ourQueenSpawn >= 0 && manhattan(c, ourQueenSpawn) > queenLeash)
            {
                // 7d: a little further out is fine where she can still turn round (a loop region longer than
                // her) and we get there first.
                int const far = manhattan(c, ourQueenSpawn);
                bool const roomy = QUEEN_TOPO_LEASH && far <= 2 * queenLeash && topo_room(c, L) && vor_margin(c) >= 2;
                if (!roomy)
                    continue;
            }
            bool clear = true;
            for (auto const& h : heads)
                if (!h.mine && manhattan(h.cell, c) <= QUEEN_PEARL_CLEAR)
                    clear = false;
            if (VORONOI && static_cast<int>(vorEnemy.size()) == N && vorEnemy[c] <= eta)
                clear = false; // A: an enemy gets there as soon as she does
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
            conf *= 1.0 + RV_PEARL_W * rv_size_factor() * rv_norm(c);
        // In survival mode she does not go near them at all.
        if (amQueen && sit.survival)
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
                conf *= my.leader ? 0.3 : 0.7;
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

// F6: is the map centre worth pulling toward? Yes while most of it is
// unseen; once seen, only if it holds spawn tiles (Australia's is empty water).
// 7a: spawn-tile density by 2D prefix sums, rebuilt once a turn: any box count in O(1) (clipped at the map edge
// like the diamond loop below).
std::vector<int> densP;
int densRound = -1;
int dens_box(int cx, int cy, int r)
{
    if (densRound != R || static_cast<int>(densP.size()) != (W + 1) * (H + 1))
    {
        densRound = R;
        densP.assign((W + 1) * (H + 1), 0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int const t = y * W + x;
                int const v = nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0;
                densP[(y + 1) * (W + 1) + x + 1] = v + densP[y * (W + 1) + x + 1] + densP[(y + 1) * (W + 1) + x] - densP[y * (W + 1) + x];
            }
    }
    int const x0 = std::max(0, cx - r), x1 = std::min(W - 1, cx + r), y0 = std::max(0, cy - r), y1 = std::min(H - 1, cy + r);
    auto P = [](int y, int x) { return densP[y * (W + 1) + x]; };
    return P(y1 + 1, x1 + 1) - P(y0, x1 + 1) - P(y1 + 1, x0) + P(y0, x0);
}

int spawn_tiles_near(int c, int r)
{
    if (DENSITY_BOX)
        return dens_box(c % W, c / W, std::max(1, r - 1)); // a (2r-1)^2 box: about the diamond's area
    int n = 0;
    int const cx = c % W, cy = c / W;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r + std::abs(dy); dx <= r - std::abs(dy); dx++)
        {
            int const x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            int const t = y * W + x;
            n += nextSpawn[t] >= 0 || pearlState[t] != 0 || gapHi[t] > 0;
        }
    return n;
}
double centre_pull()
{
    static int round = -1;
    static double val = 1.0;
    if (round == R)
        return val;
    round = R;
    int const r = std::max(3, (W + H) / 12);
    int const ctr = (H / 2) * W + W / 2;
    int seen = 0, cells = 0;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r + std::abs(dy); dx <= r - std::abs(dy); dx++)
        {
            int const x = W / 2 + dx, y = H / 2 + dy;
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            cells++;
            seen += seenRound[y * W + x] >= 0;
        }
    val = (seen * 2 < cells || spawn_tiles_near(ctr, r) >= 2) ? 1.0 : 0.0;
    return val;
}

// 6c v3 (INTENT_MEMORY 3): the head start, in steps, that a chooser gives the goal this dragon picked last round
// for the same intent - a new goal must be closer by COMMIT_MARGIN steps and by 30%. None with an enemy head in view.
double keep_bonus(int c, int kind)
{
    if (INTENT_MEMORY != 3 || amQueen || c != lastGoal || kind != lastGoalIntent || R - lastGoalR > 1 || distHead[c] >= INF)
        return 0;
    for (auto const& h : heads)
        if (!h.mine)
            return 0;
    return std::max<double>(COMMIT_MARGIN, 0.3 * distHead[c]);
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
                score -= RV_EXPLORE_W * rv_size_factor() * mid * (CENTRE_KNOWN ? centre_pull() : 1.0);
            }
            if (VORONOI && !amQueen && R >= 10)
            {
                // A: hold and push our side of the border; stay out of theirs.
                int const mg = vor_margin(c);
                bool const push = OPEN_PUSH && R < OPEN_PUSH_END; // O2: the opening claims ground
                if (mg < -3)
                    score += push ? 0.0 : VOR_DEEP_PEN;
                else if (mg >= (push ? -3 : 0) && mg <= 4)
                    score -= push ? 2 * VOR_FRONT_BONUS : VOR_FRONT_BONUS;
            }
            if (my.role == ROLE_EXPLORER)
                score -= 0.3 * dh; // explorers push further out
            for (auto const& dg : dangers)
                if (dg.cause == 1 && R - dg.round <= 30 && manhattan(dg.cell, c) <= 4)
                    score += my.leader ? 200 : 40;
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
        if (CAMP_MAX_WAIT > 0 && wait > CAMP_MAX_WAIT + distHead[c])
            continue; // F6: a long wait - explore or collect elsewhere instead
        if (VORONOI && static_cast<int>(vorEnemy.size()) == N && vorEnemy[c] + 1 < distHead[c])
            continue; // A: they get there first
        double score = distHead[c] + 0.5 * std::max(0.0, wait - distHead[c]);
        if (REGION_VALUE && !amQueen)
            score -= RV_CAMP_W * rv_size_factor() * rv_norm(c); // P7: rich areas first
        if (ownHalf)
        {
            int const hm = halfMargin(c);
            if (hm <= 0)
                continue;
            score += 0.7 * hm; // near the middle: where raiders come through
        }
        score -= keep_bonus(c, I_CAMP);
        if (score < bestScore)
        {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

// P8 outposts: the pearl-richest known spots, OUTPOST_SEP apart (2-8 of
// them by map size). An idle collector takes the nearest one that is not
// crowded by teammates already.
std::vector<int> outposts;
int outpostRound = -100;
int choose_outpost()
{
    if (R - outpostRound >= 10)
    {
        outpostRound = R;
        update_region_value();
        outposts.clear();
        int const K = mapProf.outposts;
        std::vector<int> cand;
        for (int c = 0; c < N; c++)
            if (regionVal[c] >= OUTPOST_MIN_RV && seenRound[c] >= 0 && (OUTPOST_MIN_MARGIN <= -99 || halfMargin(c) >= OUTPOST_MIN_MARGIN))
                cand.push_back(c);
        std::sort(cand.begin(), cand.end(), [](int a, int b) { return regionVal[a] > regionVal[b]; });
        if (OUTPOST_MIN_TILES > 0)
            std::erase_if(cand, [](int c) { return spawn_tiles_near(c, RV_RAD) < OUTPOST_MIN_TILES; });
        std::sort(cand.begin(), cand.end(), [](int a, int b) { return regionVal[a] > regionVal[b]; });
        for (int c : cand)
        {
            bool ok = true;
            for (int o : outposts)
                if (manhattan(o, c) < OUTPOST_SEP)
                    ok = false;
            if (ok)
                outposts.push_back(c);
            if (static_cast<int>(outposts.size()) >= K)
                break;
        }
    }
    std::vector<int> mates;
    for (auto const& h : heads)
        if (h.mine && h.id != me)
            mates.push_back(h.cell);
    for (auto const& [id, st] : fleet)
        if (id != me && R - st.round <= 6)
            mates.push_back(st.cell);
    int best = -1;
    double bestScore = 1e18;
    for (int o : outposts)
    {
        if (distHead[o] >= 40)
            continue;
        int crowd = 0;
        for (int m : mates)
            if (manhattan(m, o) <= 5)
                crowd++;
        double const sc = distHead[o] + OUTPOST_CROWD * crowd - keep_bonus(o, I_OUTPOST);
        if (sc < bestScore)
        {
            bestScore = sc;
            best = o;
        }
    }
    return best;
}

// OPEN_PLAN zones: the richest spots we know (our outposts + zones teammates
// reported). Each collector takes the zone worth most to it: richness shared
// by the teammates already working it, minus the walk - rich zones draw more
// dragons, thin ones fewer, and the split adapts every turn.
int choose_zone()
{
    choose_outpost(); // refreshes `outposts`
    std::vector<std::pair<int, int>> zs;
    for (int o : outposts)
        zs.push_back({o, spawn_tiles_near(o, RV_RAD)});
    for (auto const& z : zoneReps)
        if (R - z.round <= 80)
            zs.push_back({z.cell, z.tiles});
    std::vector<int> mates;
    for (auto const& h : heads)
        if (h.mine && h.id != me && h.id != queenId)
            mates.push_back(h.cell);
    for (auto const& [id, st] : fleet)
        if (id != me && id != queenId && R - st.round <= 6)
            mates.push_back(st.cell);
    int best = -1;
    double bestV = -1e18;
    for (auto const& [c, tiles] : zs)
    {
        if (c < 0 || c >= N || distHead[c] >= INF || distHead[c] > 60 || tiles < ZONE_TILES)
            continue;
        if (OUTPOST_MIN_MARGIN > -99 && halfMargin(c) < OUTPOST_MIN_MARGIN)
            continue;
        int assigned = 0;
        for (int m : mates)
            if (manhattan(m, c) <= ZONE_RAD)
                assigned++;
        double const v = static_cast<double>(tiles) / (assigned + 1) - ZONE_DIST_W * (distHead[c] - keep_bonus(c, I_ZONE));
        if (v > bestV)
        {
            bestV = v;
            best = c;
        }
    }
    return best;
}

// P8 wall: this member's place in the line advancing on the enemy's home -
// members spread WALL_GAP apart across the line of advance (by id rank); a
// member well ahead of the rest waits (-1) so the line arrives together.
int wall_goal()
{
    int const es0 = enemy_spawn_guess();
    if (es0 < 0 || ourQueenSpawn < 0 || body.empty())
        return -1;
    // Not clearly stronger: hold the middle line between the two homes.
    double const frac = units >= WALL_PUSH_RATIO * est_enemy_units() ? 1.0 : 0.5;
    int const es = cell_of(std::clamp(static_cast<int>(std::lround(ourQueenSpawn % W + frac * (es0 % W - ourQueenSpawn % W))), 0, W - 1),
                           std::clamp(static_cast<int>(std::lround(ourQueenSpawn / W + frac * (es0 / W - ourQueenSpawn / W))), 0, H - 1));
    std::vector<int> ids{me};
    double othersDist = 0;
    int others = 0;
    for (auto const& [id, st] : fleet)
        if (id != me && id != queenId && !st.leader && R - st.round <= 4 && wall_slot(id) && st.len >= WALL_MIN_LEN)
        {
            ids.push_back(id);
            othersDist += manhattan(st.cell, es);
            others++;
        }
    if (others > 0 && manhattan(body[0], es) < othersDist / others - WALL_LEAD)
        return -1;
    std::sort(ids.begin(), ids.end());
    int const rank = static_cast<int>(std::find(ids.begin(), ids.end(), me) - ids.begin());
    int const n = static_cast<int>(ids.size());
    double const ax = es % W - ourQueenSpawn % W, ay = es / W - ourQueenSpawn / W;
    double const al = std::max(1.0, std::sqrt(ax * ax + ay * ay));
    double const off = (rank - (n - 1) / 2.0) * WALL_GAP;
    int const gx = std::clamp(static_cast<int>(std::lround(es % W - ay / al * off)), 0, W - 1);
    int const gy = std::clamp(static_cast<int>(std::lround(es / W + ax / al * off)), 0, H - 1);
    int best = -1, bestD = INF;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
        {
            int const x = gx + dx, y = gy + dy;
            if (x < 0 || y < 0 || x >= W || y >= H)
                continue;
            int const c = y * W + x;
            if (distHead[c] >= INF)
                continue;
            int const d = std::abs(dx) + std::abs(dy);
            if (d < bestD)
            {
                bestD = d;
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

// F5: the safest open cell within 8 steps of the queen: outside every enemy
// reach (two turns if possible), far from enemy heads, open on all sides,
// near our own dragons, never in a pocket.
int choose_flee()
{
    int best = -1;
    double bestScore = -1e18;
    int const hd = body.empty() ? -1 : body[0];
    if (hd < 0)
        return -1;
    for (int c = 0; c < N; c++)
    {
        int const d = distHead[c];
        if (d < 1 || d > 8 || qThreat[c] < INF || in_pocket(c) || seenRound[c] < 0)
            continue;
        double sc = 2.0 * open_sides(c);
        if (QUEEN_LOOK2 && static_cast<int>(qThreat2.size()) == N && qThreat2[c] >= INF)
            sc += 6;
        int nearE = 99;
        for (auto const& h : heads)
            if (!h.mine)
                nearE = std::min(nearE, manhattan(h.cell, c));
        for (auto const& e : enemyReps)
            if (R - e.round <= 3)
                nearE = std::min(nearE, manhattan(e.cell, c));
        sc += 1.5 * std::min(nearE, 10);
        int mates = 0;
        for (auto const& h : heads)
            if (h.mine && h.id != me && manhattan(h.cell, c) <= 4)
                mates++;
        for (auto const& [id, st] : fleet)
            if (id != me && R - st.round <= 3 && manhattan(st.cell, c) <= 4)
                mates++;
        sc += 1.0 * std::min(mates, 4) - 0.4 * d;
        if (sc > bestScore)
        {
            bestScore = sc;
            best = c;
        }
    }
    return best;
}

// 7j: chokepoint post - the first bridge (topology) on the queen's shortest way toward the enemy side, within
// CHOKE_RANGE steps of her: the cell on her side of it. One guard there sees (and can block) everything that comes
// through. -1 when her way out has no bridge that close.
int choke_post_search(int qCell);
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
            int const n = step_open(q[i], d); // their side is mostly unexplored: plan through it (only bridges need known edges)
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

// Bunker: once the enemy queen is dead, ours only has to stay alive. A queen
// of length 2-3 can circle a 2x2 box for ever (verified in the engine; at
// length 4 her own tail blocks the loop). Boxes with few open sides are safer.
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

} // namespace
