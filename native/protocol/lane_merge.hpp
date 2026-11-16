// Lane combat (fork E33Lanes): pure merge logic, shared by the mod (host), the fake peer and the unit tests.
// No game or Windows dependency. Rules: docs/PLAN-LANES.md, "Merge rule v1".
#pragma once

#include <cstdint>
#include <vector>

#include "messages.hpp"

namespace e33::proto::lane {

// One machine's enemy changes since the last EnemyStateSync (`base`), from its local end-of-turn state.
// Enemies dead in base are skipped. An enemy alive in base but missing locally counts as killed (all its HP).
// Enemies that appeared locally (summons) are not reported: the host's own state is authoritative for them.
std::vector<EnemyDelta> deltas(const std::vector<EnemyState>& base, const std::vector<EnemyState>& local_end);

struct MergeStats {
    int results = 0;          // client results applied
    double overkill = 0;      // client damage aimed at an enemy already at 0 HP (wasted, F4 default)
    int kills = 0;            // enemies that died in this merge
};

// Host merge. host_end: the host's own end-of-turn enemy state, counted in full (its damage, but also enemy heals,
// shield gains and turn-start effects, which every machine plays and must count once). results: clients' LaneResults
// (player >= 1), applied in player order, damage only: HP and shield losses, break progress. HP is clamped to
// [0, max_hp]; an enemy at 0 HP after the merge is dead.
std::vector<EnemyState> merge(const std::vector<EnemyState>& base, const std::vector<EnemyState>& host_end,
                              std::vector<LaneResult> results, MergeStats& stats);

// FNV-1a 64 of the enemy state (spot order; HP rounded; dead enemies by spot and flag only). Desync check after repair.
uint64_t hash(const std::vector<EnemyState>& enemies);

} // namespace e33::proto::lane
