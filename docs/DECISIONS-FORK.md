# Fork decisions

Same format as upstream's `DECISIONS.md`: finding, decision, revisit if. Numbered F1, F2… Entries marked **default** apply the brief's stated default to an open question until Ryan answers it.

---

## F1: Fork identity is E33Lanes (2026-11-16)

**Finding:** two installs that share a mod folder, save folder, lobby tag or protocol magic can join, update or overwrite each other.
**Decision:** the fork is E33Lanes everywhere (table in `FORK.md`). Releases come from `ryandalton1/e33-coop`.
**Revisit if:** free roam or lane combat lands upstream and the fork is retired.

## F2: Lane merges happen before the next actor is picked (2026-11-16)

**Finding:** in the S0 probe, an enemy killed by a merge applied on the next turn's start had already been picked, and it attacked while invisible. Kills applied before the next pick were clean (`SPIKES.md`).
**Decision:** at the end of a lane turn, the local game does not advance to the next actor until the host's `EnemyStateSync` is applied. Kills from the merge use `SetDead` at that point.
**Revisit if:** the game exposes a way to drop an already picked actor from its turn.

## F3: Turn-flow hooks come from the C++ hook bus, not UE4SS Lua (2026-11-16)

**Finding:** UE4SS 3.0.1 Lua `RegisterHook` on `OnTurnStart`/`OnTurnEnded` coincided with about 12 hero turns in a row; `StartBattleEndFlow` runs mid-fight. A `LoopAsync` started inside `ExecuteInGameThread` crashed once.
**Decision:** Lua stays for probes only (property reads and setters, loops started at mod load). All lane logic uses upstream's C++ hooks.
**Revisit if:** never for the mod; possibly for future probes with a newer UE4SS.

## F4: Defaults for the brief's open questions (2026-11-16)

Defaults, applied until Ryan answers:

| Question | Default |
| --- | --- |
| Progression persistence | A co-op profile per player; never touches solo saves |
| Overkill | Wasted (no redirect) |
| Turn order | Fixed lane cycle; enemies placed by the host's initiative |
| Enemy HP multiplier | 3.0x for 3 players, host option 2.5–4.0 |
| Player count | 2 or 3; N = players (HP x N) |
| Teammate visibility in combat | Status strip and damage feed |
| After a pulled-in battle | Everyone returns to their own position |
| Upstream | Ask Killian before Phase 5 |

**Revisit if:** Ryan answers any of these.
