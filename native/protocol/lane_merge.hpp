// Lane combat (fork E33Lanes): pure merge logic, shared by the mod (host), the fake peer and the unit tests.
// No game or Windows dependency. Rules: docs/PLAN-LANES.md, "Merge rule v1" and Phase 4 (statuses, per-player marks).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "messages.hpp"

namespace e33::proto::lane {

// One machine's enemy changes since the last EnemyStateSync (`base`), from its local end-of-turn state.
// Enemies dead in base are skipped. An enemy alive in base but missing locally counts as killed (all its HP).
// Enemies that appeared locally (summons) are not reported: the host's own state is authoritative for them.
// Statuses, by class: stacks gained (> 0) or spent (< 0) go in buffs_added; a class gone entirely in buffs_removed.
std::vector<EnemyDelta> deltas(const std::vector<EnemyState>& base, const std::vector<EnemyState>& local_end);

struct MergeStats {
    int results = 0;          // client results applied
    double overkill = 0;      // client damage aimed at an enemy already at 0 HP (wasted, F4 default)
    int kills = 0;            // enemies that died in this merge
    int statuses_added = 0;   // client status gains applied
    int statuses_spent = 0;   // client status consumptions applied
    int marks_refused = 0;    // stacks a player tried to spend that belong to someone else (per-player marks)
    int marks_restored = 0;   // stacks the host's own game spent that belonged to a client (put back)
};

// Per-player marks (Sciel's Foretell and other consumable marks): stacks per (enemy spot, class) and player. Kept by the
// host across turns; a player can only spend its own stacks. Classes are "owned" when their name contains one of
// `owned` (case-sensitive substrings, e.g. "Foretell", "Mark").
struct MarkLedger {
    std::vector<std::string> owned;
    std::map<std::pair<uint8_t, std::string>, std::map<uint8_t, int32_t>> stacks;
    bool is_owned(const std::string& cls) const;
    int32_t of(uint8_t spot, const std::string& cls, uint8_t player) const;
};

inline constexpr int32_t kStackCap = 99;   // safety cap; the game's own caps apply when it reads the repaired state

// Host merge. host_end: the host's own end-of-turn enemy state, counted in full (its damage, but also enemy heals,
// shield gains and turn-start effects, which every machine plays and must count once). results: clients' LaneResults
// (player >= 1), applied in player order: HP and shield losses, break progress, status gains and consumptions. HP is
// clamped to [0, max_hp]; an enemy at 0 HP after the merge is dead. ledger: per-player marks (optional).
std::vector<EnemyState> merge(const std::vector<EnemyState>& base, const std::vector<EnemyState>& host_end,
                              std::vector<LaneResult> results, MergeStats& stats, MarkLedger* ledger = nullptr);

// FNV-1a 64 of the enemy state (spot order; HP rounded; dead enemies by spot and flag only). Desync check after repair.
// Statuses are not hashed (turn counters tick at slightly different moments on each machine).
uint64_t hash(const std::vector<EnemyState>& enemies);

} // namespace e33::proto::lane
