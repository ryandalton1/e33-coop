// Fork E33Lanes: read and edit one value of a character's stat maps (AC_jRPG_CharacterBattleStats_C.CharacterCurrentStats /
// CharacterBaseStats, TMap<E_jRPG_StatType, double>, GAME-INTERNALS.md section 9). Used for the lane HP multiplier (max HP),
// as the S0 probe did from Lua. Values are edited in place; keys are never added or removed.
#pragma once

#include <optional>

namespace RC::Unreal { class UObject; }

namespace e33::statmap {

inline constexpr int kHP = 1;      // E_jRPG_StatType: 1 = HP, 2 = AP, 3 = attack, 8 = speed, 9 = crit (Versus)
inline constexpr int kSpeed = 8;  // unverified on the game: lane turn order copies the host's per slot (lane_speeds_aligned)

std::optional<double> get(RC::Unreal::UObject* stats, const wchar_t* map, int stat);
bool set(RC::Unreal::UObject* stats, const wchar_t* map, int stat, double value);

// Max HP as the game computes it (GetMaxHP), or the CharacterCurrentStats value if the call fails.
double max_hp(RC::Unreal::UObject* stats);
// Sets max HP in both maps (base and current, in case current stats are rebuilt from base). True if current was written.
bool set_max_hp(RC::Unreal::UObject* stats, double value);

} // namespace e33::statmap
