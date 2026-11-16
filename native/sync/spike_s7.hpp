// Spike S7 (fork E33Lanes, docs/PLAN-LANES.md): find a hold between a hero's action and the game's pick of the next
// actor that keeps the game rendering. Dev option only: -e33s7=<mode>, off by default.
//   observe   log every candidate turn-end/turn-start function with frame number and time (order and gaps)
//   pause_af  observe + pause the game (SetGamePaused) in BattleMgr:OnCharacterActionFinished pre, for a hero
//   pause_ect observe + pause in BattleMgr:"End Character Turn" pre (current character is a hero)
//   pause_te  observe + pause in BattleChar:OnTurnEnded post, for a hero
// -e33s7hold=<s> hold length (default 5). Verdict per hold: did the next StartCharacterTurn/OnTurnStart wait for the
// release (s7_turns_during_hold = 0), did frames keep ticking (s7_frames_during_hold), did the next turn start after it.
#pragma once

namespace e33::spike_s7 {

void init();             // reads the option and subscribes the hooks (no-op when off)
void tick(double dt);    // engine tick: frame count, hold release
bool active();

} // namespace e33::spike_s7
