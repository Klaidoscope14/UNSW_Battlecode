// CORE - geometry on the wrapping grid, global sizes, rng, decision trace.
// Part of the fleet bot; included once, in order, by main.cpp (one translation unit).
#pragma once

namespace {


// Geometry
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

// Decision trace (local testing only): with FLEET_TRACE=<dir> set, every turn
// appends "round id length head action sonars | note" to <dir>/<team><id>.txt.
// tools/testing/trace.py compares two builds turn by turn. Off in the judge.
std::FILE* traceF = nullptr;
#ifdef QDIAG
void diag_note(char const* what) // debug: one line per event (FLEET_DIAG)
{
    if (char const* df = std::getenv("FLEET_DIAG"))
        if (std::FILE* f = std::fopen(df, "a"))
        {
            std::fprintf(f, "N %s\n", what);
            std::fclose(f);
        }
}
#define DIAG(x) diag_note(x)
#else
#define DIAG(x) ((void)0)
#endif
std::string traceAct = "fallback";

} // namespace
