#include "lane_merge.hpp"

#include <algorithm>
#include <cmath>

namespace e33::proto::lane {
namespace {

const EnemyState* find(const std::vector<EnemyState>& v, uint8_t spot)
{
    for (const auto& e : v) if (e.spot == spot) return &e;
    return nullptr;
}
EnemyState* find(std::vector<EnemyState>& v, uint8_t spot)
{
    for (auto& e : v) if (e.spot == spot) return &e;
    return nullptr;
}

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void byte(uint8_t b) { h ^= b; h *= 1099511628211ull; }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) byte(static_cast<uint8_t>(v >> (i * 8))); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
};

} // namespace

std::vector<EnemyDelta> deltas(const std::vector<EnemyState>& base, const std::vector<EnemyState>& local_end)
{
    std::vector<EnemyDelta> out;
    for (const auto& b : base) {
        if (b.dead) continue;
        EnemyDelta d;
        d.spot = b.spot;
        const EnemyState* e = find(local_end, b.spot);
        if (!e) {                                    // gone locally: killed and removed
            d.hp = -b.hp;
            d.killed = true;
        } else {
            d.hp = e->hp - b.hp;
            d.shield = e->shield - b.shield;
            d.break_bar = e->break_bar - b.break_bar;
            d.killed = e->dead || e->hp <= 0;
        }
        if (d.hp != 0 || d.shield != 0 || d.break_bar != 0 || d.killed) out.push_back(d);
    }
    return out;
}

std::vector<EnemyState> merge(const std::vector<EnemyState>& base, const std::vector<EnemyState>& host_end,
                              std::vector<LaneResult> results, MergeStats& stats)
{
    // Start from the host's end state: the host's own delta counts in full.
    std::vector<EnemyState> out = host_end;
    for (const auto& b : base)                       // enemy removed on the host (killed and despawned): keep it, dead
        if (!find(out, b.spot)) { EnemyState e = b; e.hp = 0; e.dead = true; out.push_back(e); }
    std::sort(out.begin(), out.end(), [](const EnemyState& a, const EnemyState& b) { return a.spot < b.spot; });

    std::stable_sort(results.begin(), results.end(), [](const LaneResult& a, const LaneResult& b) { return a.player < b.player; });
    for (const auto& r : results) {
        if (r.player == 0) continue;                 // the host's own result is host_end
        ++stats.results;
        for (const auto& d : r.enemies) {
            EnemyState* e = find(out, d.spot);
            const EnemyState* b = find(base, d.spot);
            if (!e || !b || b->dead) continue;       // unknown enemy, or already dead before this turn
            const double dmg = d.hp < 0 ? -d.hp : 0.0;
            if (dmg > 0) {
                if (e->dead || e->hp <= 0) {
                    stats.overkill += dmg;
                } else {
                    const double applied = std::min(dmg, e->hp);
                    stats.overkill += dmg - applied;
                    e->hp -= applied;
                }
            }
            if (d.shield < 0) e->shield = std::max(0, e->shield + d.shield);
            if (d.break_bar > 0) e->break_bar += d.break_bar;
        }
    }

    for (auto& e : out) {
        const EnemyState* b = find(base, e.spot);
        if (e.dead) e.hp = 0;                        // dead on the host whatever its HP field says
        if (e.max_hp > 0) e.hp = std::min(e.hp, e.max_hp);
        if (e.hp <= 0) {
            e.hp = 0;
            if (!e.dead) { e.dead = true; if (!b || !b->dead) ++stats.kills; }
            else if (b && !b->dead) ++stats.kills;   // killed on the host this turn
        }
    }
    return out;
}

uint64_t hash(const std::vector<EnemyState>& enemies)
{
    std::vector<const EnemyState*> v;
    for (const auto& e : enemies) v.push_back(&e);
    std::sort(v.begin(), v.end(), [](const EnemyState* a, const EnemyState* b) { return a->spot < b->spot; });
    Fnv f;
    for (const auto* e : v) {
        f.byte(e->spot);
        f.byte(e->dead ? 1 : 0);
        if (e->dead) continue;
        f.i64(std::llround(e->hp));
        f.i64(std::llround(e->max_hp));
        f.i64(e->shield);
        f.i64(e->break_bar);
    }
    return f.h;
}

} // namespace e33::proto::lane
