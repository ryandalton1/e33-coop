# Spikes

## S0 LaneProbe: one-PC viability test for lane combat (2026-11-16)

**Verdict: go.** On one machine, with no networking, the game accepts everything the lane model needs from outside the local fight: enemy HP scaled 3x, damage written by someone else, and kills written by someone else.

**Setup.** Steam build 23765773, UE4SS 3.0.1, Lua mod `lua/LaneProbe` (v5). On F7 in a fight, every enemy gets 3x max and current HP (`CharacterCurrentStats[1]` and `CharacterBaseStats[1]`, then `SetHP`). Two "phantom teammates" then deal 2x every HP drop within 0.1 s (`SetHP`, and `SetDead` when HP reaches 0).

**Findings**

| Check | Result | Evidence |
|---|---|---|
| 3x max and current HP | OK on all 12 enemies scaled across 5 runs, including an endgame boss (2.36M to 7.09M). Max HP never reverted mid-fight. | `SCALE ... OK`, `MAX_HP_CHANGED` never logged (v1) |
| External damage | Accepted every time; the health bar updates instantly. | 28 phantom hits for 2.97M damage (v3), 7 hits (v5), `sethp_fallbacks=0` |
| External kills | The enemy dies, leaves the turn order and never acts again, provided the kill lands before the game picks the next actor. | v5: 2 phantom kills, `dead_actor_turns=0` |
| Turn order | Not affected by the probe. | v5: 3 hero turns, 3 enemy turns, normal order |

**Constraints for Feature A**

- **F-a. Merge before the next actor is picked.** v1 applied phantom damage on the next turn's start. An enemy killed that way had already been picked, and it attacked while invisible. Lane combat must hold the turn advance until the host's `EnemyStateSync` has been applied. The brief already makes this the sync point; it is now a hard requirement.
- **F-b. Don't build on UE4SS Lua hooks for turn events.** v2's `RegisterHook` on `OnTurnStart`/`OnTurnEnded` coincided with about 12 hero turns in a row. Also, `StartBattleEndFlow` runs mid-fight, not only at the end. Use upstream's C++ hook bus.
- **F-c. Lua `LoopAsync` started inside an `ExecuteInGameThread` callback crashed once** (C++ exception in msvcp140, v4). Start loops at mod load. Lua probe only; the real mod is C++.

**Not covered yet**

- A phantom kill of the *last* enemy ending the fight with the normal victory screen and rewards.
- Shields, break bar and statuses under external writes (upstream's repair already writes them; S1 proper).
- Real network latency in the merge, and enemy turns played against each player's own heroes (S4).
