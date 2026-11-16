// Fork E33Lanes: party profiles (docs/PLAN-LANES.md Phase 4, DECISIONS-FORK F4/F5). In lane combat each player brings
// their own heroes and builds, while the world still comes from the host's snapshot (upstream D12).
//  - Export: the characters' states as the game serializes them (game::make_characters_blob, upstream's build blobs)
//    plus the party order, saved to SaveGames/e33lanes/profile/. Automatic the first time a solo (non co-op) game is
//    played with the mod; Ctrl+F9 re-exports from the loaded solo save at any time.
//  - Lane battle (client): the world's characters are kept in memory, the profile is applied (LoadCharacterState) and the
//    party set to the profile's before the battle spawns its heroes.
//  - Back in exploration: the characters (with this battle's XP) become the new profile (co-op progression lives in the
//    profile, never in the solo save), then the world's characters and party are restored, before the builds module
//    looks at them (the host's world is unchanged for everyone).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace e33::profile {

void init();
// Game thread, every frame. in_session: in a co-op session; coop_world: the loaded game is a co-op copy (not a solo save).
void tick(double now, bool in_session, bool coop_world);
bool have();                                   // a profile exists on disk
bool export_now(std::string& err);             // from the loaded game (refused in a co-op copy)
// Client, lane battle about to start (idempotent per battle): profile applied. False if none or it failed.
bool apply_for_battle(uint32_t battle_id);
bool applied();
// Party for the battle: the profile's when applied, else `fallback` (the host's).
std::vector<std::string> battle_party(const std::vector<std::string>& fallback);
void on_battle_end(int result);                // StartBattleEndFlow: restore scheduled for the return to exploration

} // namespace e33::profile
