// Unit tests for the lane combat merge (fork E33Lanes, native/protocol/lane_merge). No game. Run in CI via ctest.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../native/protocol/lane_merge.hpp"

using namespace e33::proto;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static EnemyState enemy(uint8_t spot, double hp, double max_hp, int32_t shield = 0, int64_t brk = 0, bool dead = false)
{
    EnemyState e; e.spot = spot; e.cls = "BP_Enemy"; e.hp = hp; e.max_hp = max_hp; e.shield = shield; e.break_bar = brk; e.dead = dead;
    return e;
}
static LaneResult result(uint8_t player, std::vector<EnemyDelta> d) { LaneResult r; r.battle_id = 1; r.turn = 3; r.player = player; r.enemies = std::move(d); return r; }
static const EnemyState& at(const std::vector<EnemyState>& v, uint8_t spot) { for (auto& e : v) if (e.spot == spot) return e; static EnemyState none; return none; }

// Three players all hit the same enemy: host 1000 (in host_end), clients 2000 and 500.
static void test_sum()
{
    const std::vector<EnemyState> base = {enemy(0, 30000, 30000), enemy(1, 9000, 9000)};
    const std::vector<EnemyState> host = {enemy(0, 29000, 30000), enemy(1, 9000, 9000)};
    lane::MergeStats st;
    auto m = lane::merge(base, host, {result(2, {{0, -500, 0, 0, false}}), result(1, {{0, -2000, 0, 0, false}})}, st);
    CHECK(at(m, 0).hp == 26500);
    CHECK(at(m, 1).hp == 9000);
    CHECK(st.results == 2 && st.kills == 0 && st.overkill == 0);
}

// Client heals and gains are ignored (every machine plays the same enemy turn); the host's count once.
static void test_heal_counted_once()
{
    const std::vector<EnemyState> base = {enemy(0, 10000, 30000, 2)};
    const std::vector<EnemyState> host = {enemy(0, 12000, 30000, 3)};   // enemy healed 2000 and gained a shield point
    lane::MergeStats st;
    auto m = lane::merge(base, host, {result(1, {{0, +2000, +1, 0, false}})}, st);
    CHECK(at(m, 0).hp == 12000);
    CHECK(at(m, 0).shield == 3);
}

// Host kills the enemy; the client's damage on it is wasted (overkill).
static void test_overkill()
{
    const std::vector<EnemyState> base = {enemy(0, 5000, 30000)};
    const std::vector<EnemyState> host = {enemy(0, 0, 30000, 0, 0, true)};
    lane::MergeStats st;
    auto m = lane::merge(base, host, {result(1, {{0, -4000, 0, 0, true}})}, st);
    CHECK(at(m, 0).dead && at(m, 0).hp == 0);
    CHECK(st.overkill == 4000);
    CHECK(st.kills == 1);
}

// Neither player kills alone; together they do (the S0 probe's phantom kill).
static void test_combined_kill()
{
    const std::vector<EnemyState> base = {enemy(1, 4540, 6963)};
    const std::vector<EnemyState> host = {enemy(1, 2117, 6963)};       // host dealt 2423
    lane::MergeStats st;
    auto m = lane::merge(base, host, {result(1, {{1, -2423, 0, 0, false}}), result(2, {{1, -2423, 0, 0, false}})}, st);
    CHECK(at(m, 1).dead && at(m, 1).hp == 0);
    CHECK(st.kills == 1);
    CHECK(std::fabs(st.overkill - (2423 * 2 - 2117)) < 1e-6);
}

// Shields never go below 0; break progress adds up; HP is clamped to max.
static void test_shield_break_clamp()
{
    const std::vector<EnemyState> base = {enemy(0, 100, 200, 2, 100)};
    const std::vector<EnemyState> host = {enemy(0, 250, 200, 1, 150)};  // host-side value above max (heal overflow)
    lane::MergeStats st;
    auto m = lane::merge(base, host, {result(1, {{0, 0, -5, 40, false}})}, st);
    CHECK(at(m, 0).shield == 0);
    CHECK(at(m, 0).break_bar == 190);
    CHECK(at(m, 0).hp == 200);
}

// The host's own result (player 0) is never applied twice; enemies already dead before the turn stay untouched.
static void test_ignored_inputs()
{
    const std::vector<EnemyState> base = {enemy(0, 1000, 1000), enemy(1, 0, 1000, 0, 0, true)};
    const std::vector<EnemyState> host = {enemy(0, 900, 1000), enemy(1, 0, 1000, 0, 0, true)};
    lane::MergeStats st;
    auto m = lane::merge(base, host, {result(0, {{0, -100, 0, 0, false}}), result(1, {{1, -500, 0, 0, false}, {7, -1, 0, 0, false}})}, st);
    CHECK(at(m, 0).hp == 900);
    CHECK(at(m, 1).dead && st.kills == 0 && st.overkill == 0);
    CHECK(st.results == 1);
}

// An enemy the host despawned stays in the merged state, dead.
static void test_host_removed()
{
    const std::vector<EnemyState> base = {enemy(0, 1000, 1000), enemy(2, 500, 500)};
    const std::vector<EnemyState> host = {enemy(0, 1000, 1000)};
    lane::MergeStats st;
    auto m = lane::merge(base, host, {}, st);
    CHECK(m.size() == 2 && at(m, 2).dead && st.kills == 1);
}

static void test_deltas()
{
    const std::vector<EnemyState> base = {enemy(0, 1000, 1000, 2, 10), enemy(1, 500, 500), enemy(2, 0, 500, 0, 0, true), enemy(3, 800, 800)};
    const std::vector<EnemyState> local = {enemy(0, 700, 1000, 1, 60), enemy(1, 0, 500, 0, 0, true), enemy(2, 0, 500, 0, 0, true), enemy(9, 50, 50)};
    auto d = lane::deltas(base, local);
    // spot 0 damaged, spot 1 killed, spot 2 already dead (skipped), spot 3 missing locally (killed), spot 9 new (not reported)
    CHECK(d.size() == 3);
    CHECK(d[0].spot == 0 && d[0].hp == -300 && d[0].shield == -1 && d[0].break_bar == 50 && !d[0].killed);
    CHECK(d[1].spot == 1 && d[1].hp == -500 && d[1].killed);
    CHECK(d[2].spot == 3 && d[2].hp == -800 && d[2].killed);
    CHECK(lane::deltas(base, base).empty());
}

static void test_hash()
{
    std::vector<EnemyState> a = {enemy(0, 1000.2, 2000), enemy(1, 0, 500, 0, 0, true)};
    std::vector<EnemyState> b = {enemy(1, 0, 500, 0, 0, true), enemy(0, 999.9, 2000)};   // other order, same rounding
    CHECK(lane::hash(a) == lane::hash(b));
    b[1].shield = 1;
    CHECK(lane::hash(a) != lane::hash(b));
    std::vector<EnemyState> c = a; c[1].hp = 123;   // a dead enemy's HP field does not matter
    CHECK(lane::hash(a) == lane::hash(c));
}

int main()
{
    test_sum();
    test_heal_counted_once();
    test_overkill();
    test_combined_kill();
    test_shield_break_clamp();
    test_ignored_inputs();
    test_host_removed();
    test_deltas();
    test_hash();
    std::printf(g_fail ? "%d FAILURE(S)\n" : "all lane tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
