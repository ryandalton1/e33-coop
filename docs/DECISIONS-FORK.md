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

## F5: v1 uses a copy of the host's party; own parties are a committed Phase 4 feature (2026-11-16)

**Finding:** a joining player loads the host's save (upstream D5/D12), so their heroes are the host's characters. Bringing your own heroes needs party profiles (spike S2) and build application.
**Decision:** lane combat v1 gives every player a copy of the host's party, each playing their own copy. **Party profiles (each player brings their own heroes and builds) are required for Phase 4's exit**, not optional: Ryan confirmed it is a must-have feature.
**Revisit if:** S2 shows `OverrideParty` plus build setters can't apply a foreign party; then find another path, don't drop the feature.

## F6: Lane combat plan approved (2026-11-16)

**Decision:** `PLAN-LANES.md` approved by Ryan as written. Work starts with spike S7 (a non-freezing hold before the next actor) and the lane protocol messages.

## F7: Full code written ahead of the spikes; choices made without game tests (2026-11-16)

**Context:** Ryan asked for all the code at once. Everything below compiles in CI but none of it has run in the game. Each item names the counter or scenario that will confirm or refute it.

| Area | Choice | Check |
| --- | --- | --- |
| Hold before the next actor | Blocking `wait_for` (frozen frame), one function `lane_hold()` | S7 picks the replacement |
| Dead actor picked (F2) | Host replaces it with its lowest alive hero; clients follow `next` | `lane_dead_actor_replaced` |
| Last enemy killed by the merge | `BattleManager.CheckBattleEnd` called on every machine | `lane_battle_end_checks`, S1 |
| Statuses | Union by class, stacks summed (cap 99), consumptions in player order; applied through upstream's buff repair | `lane_status_repairs`, `lane_statuses_*` |
| Per-player marks | Classes whose name contains `Foretell` or `Mark` (`-e33lanemarks`) are tracked per player; spending another player's stacks is refused, the host's own over-spend is put back | `lane_marks_refused`, `lane_marks_restored`, unit tests |
| LaneIntent | Upstream's `IntentPreview` reused; host relays it with `spot = 0x80 + player`; shown as a status line, never mirrored | `lane_intents_shown` |
| Teammate strip, damage feed | Status line from `HeroStatus`; toast with the teammates' share of the merged damage | `lane_feed_shown` |
| Spectate | A wiped client's local fight ends (game's defeat screen, local choice); the host stops waiting for it. **Host wipe still ends the fight for everyone** (no way found yet to keep the host's fight alive) | `lane_spectates`, `lane_players_wiped` |
| Turn order | The host's own initiative decides; clients force their own hero in that slot (lowest alive hero if it is down). The brief's strict lane 1-2-3 cycle is not enforced | `lane_actors_forced`, `lane_slot_fallbacks` |
| Party profiles | Upstream's character blobs: exported from the first solo game (or Ctrl+F9), applied before each lane battle on clients, progression saved back to the profile after a victory, world characters restored before the builds module looks | `profile_*` counters; needs a real client |
| Free roam | Everyone pilots; puppets spawned with `BeginDeferredActorSpawnFromClass` from the sender's pawn class, flying mode, moved by teleport each frame, velocity written for the anim graph; flags from anyone; replays off; no snapshot reload; battles requested with the encounter name; cinematics gather, dialogues stay local; Ctrl+F6 travel | `roam_puppet_host` scenario (S5), `roam_*` counters |
| Rewards | Upstream's host-rolled rewards unchanged; XP goes to the local party (the profile's on clients) | profile progression after a victory |

**Not done:** a fake host that merges (client-side scenario needs a recording from a real lane run), per-enemy status ownership beyond the name filter, 3-player scenario (the runner starts one fake peer), extra-turn effects (counted only through actor mismatches).
**Revisit if:** any check above fails in the scenarios or the first real session.

## F8: The game never freezes; wiped teams stay and watch (2026-11-16)

**Ryan:** "i don't want game to freeze ever. but i do want player's to wait for each others turns to finish" and "let teams that wipe stay in the fight watching it until either they win or all parties wipe".
**Decision (replaces F7's hold, spectate and turn-order rows):**
- **Wait without freezing:** no blocking wait anywhere in lane mode. At the end of a turn each machine reports; the next turn is held at its start with the game's own turn-start dependency (`BM.TurnStartNotifyDependencies`, upstream D28: the game keeps animating, a "Waiting for …" line shows who is slow). The host merges in the tick as soon as every live result is in (30 s: merged without the silent player), then everyone is released. If the merge killed the held enemy, its turn is ended (`End Character Turn`).
- **Spectate:** `IsAFullTeamDead` is answered "no" while our heroes are all down, an enemy stands and another team still fights. The wiped team stays in the fight, held, its enemy bars following the merged state, until a victory (rewards for everyone) or until every team is down (the host lets the game see it: defeat, D33). Works for the host too: a wiped host keeps merging the clients' turns.
- **Turn order:** clients copy the host's hero speeds slot by slot (`HeroStatus.speed`), so every machine's own initiative picks the same next actor without waiting; differences are counted (`lane_actor_mismatches`).
**Checks:** `lane_holds_registered` vs `lane_holds_missed` (did the game hold?), `lane_wait_max_ms`, `lane_spectates` vs `lane_wipe_suppress_missed` and `lane_all_heroes_killed_seen` (did the "no" keep us in?), `lane_speeds_aligned`, `lane_actor_mismatches`, `lane_dead_actor_turns_ended`.
**S7 is now only a diagnostic:** it measures where else the game could be held; not needed for the design above unless a check fails.

### F8 update: first in-game runs (2026-11-16)

- **Hold works:** lane_v1_host passed its lane checks on Ryan's PC: 19 turns, 18 holds registered and released, longest hold 22 ms, victory. One bug found and fixed first: releasing the hold made the game Reset the counter re-entrantly and we registered a second hold nobody released (enemy turn stuck). State is now cleared before `SolveDependency`.
- **Spectate lever changed:** the game never asks `IsAFullTeamDead`; defeat is decided inside `CheckBattleEnd` from its own lists. New lever: while a wiped team should spectate, `IsBattleEnding` is answered "yes" inside `CheckBattleEnd` (an end looks under way, none is started). Checked by `lane_spectate_host` (host wipes, fake client keeps playing with `drive_s`, expected victory).
- **Silent player:** waited for once (30 s), then marked absent and not waited for until a result of theirs comes in again (`lane_players_absent`, `lane_players_back`).
- **No wait limit by default (Ryan, 2026-11-16):** "im fine allowing one player to stall forever, this mod is played with friends in a discord call". Players wait for each other without limit. A dropped connection still ends the wait (the player leaves the game). `-e33lanewait=S` restores a limit (the silent-player test uses 30).

## F9: Free roam shares the host's story, modelled on Online Expedition (2026-11-16)

Ryan asked to "mimic the way online expedition does shared story progress" (its Nexus 820 feature list, pasted 2026-11-16), after rejecting per-player stories.

- **Why not separate stories:** with each player's own story, a story boss would be fought once per player, and a cutscene that ends in a fight cuts off friends watching the same scene.
- **Why not story-only gathering:** probe 2026-11-16 (tools/dev/probe-defeat.ps1 -Talk, new game, Lumière): all five conversations, story and side, come through the same `ActivateDialogue`. There is no story/side marker; story ones only carry their own script data.
- **Model (Online Expedition):**
  - The host's story is the party's: quests, interacted objects, map sections and flags follow the host. A follower's world actions count for the party.
  - Cutscenes play for everyone, whoever triggers them. Online Expedition skips by vote.
  - Conversation lines play for the whole party, with no gathering.
  - Followers travel with the host's zone changes; F6 teleports to the host.
  - Fights pull everyone in (already built, lanes).
- **Phase 1 (this commit):**
  - Only the host publishes map changes; followers travel with them.
  - SceneStart and SceneSkip are accepted from anyone and replayed everywhere, after the existing gather.
  - Any player's skip skips for all (a vote is phase 2).
- **Phase 2 (done the same day, Ryan: "just do all phases"):**
  - Interactions from anyone are relayed by the host. Every machine replays only world-state objects (chests, pickups: `BP_Chest_Regular`, `BP_InteractionObjectBase`), never NPCs, enemies, movement aids, trigger zones or documents.
  - Followers are repaired in place from the host: gold, inventory, flags, and beaten enemies (new: `SetBattledEnemy` for the host's ids). Parts that can't be repaired (interacted objects, GPE states and others) are only logged in roam. There is no snapshot reload in roam, because it would teleport the player.
  - Conversation lines: probe 2026-11-16 (`probe_lines.lua`) found `WBP_DialogueOverlay_C:SetDialogueLines(text, speaker)` for every line, then `PlaySoundForCharacter(SoundWave, actor)`. Each line goes out as `DialogueLine` (protocol v9) with its voice asset. Others see "[player] speaker: text" and hear the voice (`PlaySound2D`), unless they are in their own scene or a fight.
  - Skip: a hook cannot cancel `SkipCinematic`, so there is no true vote. A skip skips only your own screen; the others get "X skipped (waiting for you)", and the narrative barrier makes the skipper wait. Nobody's scene is cut by someone else.
  - Fights wait for a player's own cutscene or conversation to end. A follower's request reaching a host in a scene is queued until the scene ends.
- **Kept from the fork, unlike Online Expedition:** lane combat with each player's own party (party profiles), not a borrowed host party.
