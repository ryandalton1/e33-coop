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

struct ClassSum { int32_t stacks = 0; int32_t turns = 0; bool permanent = false; };

std::map<std::string, ClassSum> by_class(const std::vector<BuffState>& buffs)
{
    std::map<std::string, ClassSum> out;
    for (const auto& b : buffs) {
        auto& c = out[b.cls];
        c.stacks += std::max<int32_t>(1, b.stacks);
        c.turns = std::max(c.turns, b.turns);
        c.permanent = c.permanent || b.permanent;
    }
    return out;
}

// Stacks of one class on an enemy (instances summed; an instance counts at least 1).
int32_t stacks_of(const EnemyState& e, const std::string& cls)
{
    int32_t n = 0;
    for (const auto& b : e.buffs) if (b.cls == cls) n += std::max<int32_t>(1, b.stacks);
    return n;
}

// Sets one class to `n` stacks (one instance), keeping the longest duration; removes it at 0.
void set_stacks(EnemyState& e, const std::string& cls, int32_t n, int32_t turns, bool permanent)
{
    int32_t keep_turns = turns;
    bool keep_perm = permanent;
    for (const auto& b : e.buffs) if (b.cls == cls) { keep_turns = std::max(keep_turns, b.turns); keep_perm = keep_perm || b.permanent; }
    e.buffs.erase(std::remove_if(e.buffs.begin(), e.buffs.end(), [&](const BuffState& b) { return b.cls == cls; }), e.buffs.end());
    if (n > 0) e.buffs.push_back({cls, std::min(n, kStackCap), keep_turns, keep_perm});
}

} // namespace

bool MarkLedger::is_owned(const std::string& cls) const
{
    for (const auto& o : owned) if (!o.empty() && cls.find(o) != std::string::npos) return true;
    return false;
}

int32_t MarkLedger::of(uint8_t spot, const std::string& cls, uint8_t player) const
{
    auto it = stacks.find({spot, cls});
    if (it == stacks.end()) return 0;
    auto jt = it->second.find(player);
    return jt == it->second.end() ? 0 : jt->second;
}

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
            const auto bc = by_class(b.buffs), ec = by_class(e->buffs);
            for (const auto& [cls, now] : ec) {
                auto it = bc.find(cls);
                const int32_t before = it == bc.end() ? 0 : it->second.stacks;
                if (now.stacks != before) d.buffs_added.push_back({cls, now.stacks - before, now.turns, now.permanent});
            }
            for (const auto& [cls, was] : bc)
                if (!ec.count(cls)) d.buffs_removed.push_back(cls);
        }
        if (d.hp != 0 || d.shield != 0 || d.break_bar != 0 || d.killed || !d.buffs_added.empty() || !d.buffs_removed.empty()) out.push_back(d);
    }
    return out;
}

std::vector<EnemyState> merge(const std::vector<EnemyState>& base, const std::vector<EnemyState>& host_end,
                              std::vector<LaneResult> results, MergeStats& stats, MarkLedger* ledger)
{
    // Start from the host's end state: the host's own delta counts in full.
    std::vector<EnemyState> out = host_end;
    for (const auto& b : base)                       // enemy removed on the host (killed and despawned): keep it, dead
        if (!find(out, b.spot)) { EnemyState e = b; e.hp = 0; e.dead = true; e.buffs.clear(); out.push_back(e); }
    std::sort(out.begin(), out.end(), [](const EnemyState& a, const EnemyState& b) { return a.spot < b.spot; });

    // Per-player marks, host side: gains are the host's; spending more than its own puts the others' stacks back.
    if (ledger) {
        for (const auto& b : base) {
            if (b.dead) continue;
            EnemyState* e = find(out, b.spot);
            if (!e) continue;
            std::map<std::string, int32_t> classes;
            for (const auto& x : b.buffs) if (ledger->is_owned(x.cls)) classes[x.cls] = 0;
            for (const auto& x : e->buffs) if (ledger->is_owned(x.cls)) classes[x.cls] = 0;
            for (const auto& [cls, unused] : classes) {
                (void)unused;
                const int32_t delta = stacks_of(*e, cls) - stacks_of(b, cls);
                auto& mine = ledger->stacks[{b.spot, cls}][0];
                if (delta >= 0) { mine += delta; continue; }
                const int32_t spend = -delta;
                const int32_t own = std::min(spend, mine);
                mine -= own;
                if (spend > own && !e->dead) {       // the host's game spent a client's stacks: give them back
                    int32_t turns = 0; bool perm = false;
                    for (const auto& x : b.buffs) if (x.cls == cls) { turns = std::max(turns, x.turns); perm = perm || x.permanent; }
                    set_stacks(*e, cls, stacks_of(*e, cls) + (spend - own), turns, perm);
                    stats.marks_restored += spend - own;
                }
            }
        }
    }

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
            if (e->dead || e->hp <= 0) continue;     // statuses on a dead enemy: nothing to merge
            // Statuses, in player order against the merged state. Spending what no longer exists is dropped.
            for (const auto& a : d.buffs_added) {
                const int32_t cur = stacks_of(*e, a.cls);
                if (a.stacks > 0) {
                    set_stacks(*e, a.cls, cur + a.stacks, a.turns, a.permanent);
                    if (ledger && ledger->is_owned(a.cls)) ledger->stacks[{d.spot, a.cls}][r.player] += a.stacks;
                    ++stats.statuses_added;
                } else if (a.stacks < 0 && cur > 0) {
                    int32_t spend = std::min(-a.stacks, cur);
                    if (ledger && ledger->is_owned(a.cls)) {
                        auto& mine = ledger->stacks[{d.spot, a.cls}][r.player];
                        const int32_t own = std::min(spend, mine);
                        stats.marks_refused += spend - own;
                        spend = own;
                        mine -= own;
                    }
                    if (spend > 0) { set_stacks(*e, a.cls, cur - spend, a.turns, a.permanent); ++stats.statuses_spent; }
                }
            }
            for (const auto& cls : d.buffs_removed) {
                const int32_t cur = stacks_of(*e, cls);
                if (cur <= 0) continue;
                int32_t spend = cur;
                if (ledger && ledger->is_owned(cls)) {
                    auto& mine = ledger->stacks[{d.spot, cls}][r.player];
                    spend = std::min(cur, mine);
                    stats.marks_refused += cur - spend;
                    mine -= spend;
                }
                if (spend > 0) { set_stacks(*e, cls, cur - spend, 0, false); ++stats.statuses_spent; }
            }
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
            if (ledger) for (auto it = ledger->stacks.begin(); it != ledger->stacks.end();) it = it->first.first == e.spot ? ledger->stacks.erase(it) : std::next(it);
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
