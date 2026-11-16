# Lane combat: implementation plan

Status: approved by Ryan, 2026-11-16 (F6). Covers the brief's Phases 2–4 (spikes, lane combat v1, lane combat full). Upstream code references are from the fork at `a8e7fbd` (upstream `6f05893`).

## What we're starting from

- **Session:** host plus 2 clients already works (`kMaxClients = 2`, `coop_internal.hpp:83`; scenario `p3_three_host.json` runs 2 fake clients). The network layer needs no structural change.
- **Messages:** a new message type is 6 mechanical edits in `protocol/messages.{hpp,cpp}` plus a round-trip sample in `tools/tests/test_protocol.cpp`. Protocol goes to v7 for lane mode.
- **Enemy state:** `StateSync`/`UnitState` and `state.cpp` (snapshot, hash, diff, repair) can be reused for an enemy-only sync, with two additions: max HP (for the HP multiplier) and `SetHP` instead of a raw `CurrentHP` write, so the health bar updates (S0).
- **Turn control:** `BattleMgr:StartCharacterTurn` pre-hook (`coop_hooks.cpp:84`) is the earliest hook in the turn advance. The actor is already picked there, but the `Character` param can be overwritten. The only proven wait there is `wait_for`, which **freezes the game thread** (no rendering) for its duration.
- **Replay machinery to retire in lane mode:** ownership per spot, `ActionCommand`/`ComboResult`/`FreeAimEvent`/`DefenseRequest`/`DefenseResult` relay, intent and menu mirror, input lock, D28/D36 alignment. Every local hero is yours. This stays in the code behind a lane-mode switch, so upstream rebases stay tractable.

## Key design points

1. **The sync point (F2).** A lane turn must not hand the game its next actor until the host's merged enemy state is applied. Upstream can only wait there by freezing the frame. The wait lasts as long as the slowest teammate's action (seconds), so a frozen frame is not acceptable for play, though it is fine for automated tests. Spike S7 looks for a hold that keeps rendering. Fallback order:
   - (a) a hook at the end of an action (`End Character Turn`, `OnCharacterActionFinished`, turn-end events; never hooked so far) plus `SetGamePaused`, the way upstream holds the first turn (`explore::hold_pause`);
   - (b) an extra local "wait" turn: override the actor with a no-op until the merge lands;
   - (c) the freezing `wait_for` with a "waiting for teammates" banner drawn just before it.
2. **Enemy turns don't need identical AI.** Each machine plays the enemy's attack against its own heroes, and hero damage stays local. If a machine's enemy picks a different skill (its AI reads different hero HP), only that player's own board is affected. The host repairs the enemy's own state afterwards. So divergence is logged as a counter (`enemy_skill_mismatch`), not treated as a desync. The host forces only the target **slot** (targeting hooks at `coop_hooks.cpp:719`, changed from "random alive hero" to "the host's slot, else a seeded pick among your live heroes").
3. **Merge rule v1** (HP, shields, break):
   - Each machine reports its enemy deltas for the turn, measured from the last `EnemyStateSync` to the sync point.
   - From clients, only damage counts: HP and shield drops, break progress.
   - The host's own delta counts in full, including enemy heals and shield gains. Effects at an enemy's turn start (burn ticks, regen) are therefore counted once.
   - Kills: HP ≤ 0 after the merge → `SetDead` on every machine at the sync point.
4. **Heroes in v1.** Clients load the host's save (D5/D12), so their "own" heroes are the host's characters. v1 accepts that: every player fights with a copy of the host's party, each playing their own copy. Per-player parties (party profiles, S2) come in Phase 4.
5. **RNG.** Enemy-side draws keep upstream's seeded streams, re-keyed by the host's `(battle, cycle, slot or enemy spot)` instead of the local turn counter. Hero-side draws (crits, buffs, mechanics) become local and are excluded from `compare_draws`, so they don't raise false mismatches.

## Phases

### Phase 2: spikes (test-runner scenarios, results in `SPIKES.md`)

| Spike | Question | Exit |
| --- | --- | --- |
| S1 | Enemy state written from outside: 3x max/current HP, shields, break, statuses, `SetDead`; **a merge kill of the last enemy ends the fight** (not covered by S0) | Fight ends normally with rewards; UI correct |
| S3 | Per-turn enemy deltas from a snapshot diff match the observed change: multi-hit skills, free aim, Lumina procs, counters, burn | Diff equals the sum of hits on 20 scripted turns |
| S4 | Force the target slot of an enemy attack from the host on both machines; measure how often skill choice diverges | Slot forced 100%; divergence rate recorded |
| S7 | A hold before the next actor pick that keeps rendering (options a–c above) | Chosen option holds 10 s with no freeze, then the next actor plays |

S2 (party profiles) moves to Phase 4. S5 and S6 (free roam) wait for Phase 5.

### Phase 3: lane combat v1 (2 players)

1. **Lane mode switch:** a host option `lanes = 1`, sent in `BattleStart`. All replay paths (`controls()`, relays, input lock, intent mirror) short-circuit when it is on.
2. **Messages:**
   - `LaneResult{battle, turn, player, per-enemy hp/shield/break deltas, kills}`
   - `EnemyStateSync{battle, turn, cycle, enemies[] (state + max HP + buffs), next actor, target slot, hash}`
   - (`LaneStart` and `EnemyTurn` were folded into `EnemyStateSync`: one host message per sync point; done in `09c7929`)
   - `HeroStatus{player, heroes' hp/ap/alive}`
3. **Battle start:** the host applies the HP multiplier (`hp_mult`, default = number of players, host option 2.5–4.0 for 3 players) to every enemy and includes it in the first `EnemyStateSync`.
4. **Turn cycle:** fixed lane 1 → 2 → 3, with enemies placed by the host's initiative. At each sync point, the actor is overwritten with the host's next actor: the lane-k hero (or a skip if dead) or the scheduled enemy.
5. **Sync point:** at the end of each turn, send `LaneResult`; the host merges and broadcasts `EnemyStateSync`; every machine repairs, kills and sets the next actor (S7's hold).
6. **Defeat:** a wiped player spectates; the fight is lost only when everyone is wiped (host runs D33's defeat flow).
7. **Fake peer:** `--lanes` mode. On each `EnemyStateSync` naming a hero lane it sends a scripted or synthetic `LaneResult` and consumes `EnemyStateSync`.
8. **Counters:** `lane_results_merged`, `lane_timeouts`, `enemy_state_repairs`, `overkill_wasted`, `enemy_skill_mismatch`, `sync_hold_ms`.
9. **Scenarios:** `lanes_host_2p` (real host + 1 fake client), `lanes_client_2p` (real client + fake host recording), plus a last-enemy merge kill and a timeout.

**Status (2026-11-16, commits `b9d6d92`, `15a8cf1` and the scenarios commit):**
- Done (untested in game): items 1, 2, 3 (both sides scale on turn 1; the host's sync carries absolute HP), 5 (blocking hold for now, `lane_hold()` in `coop_lanes.cpp` is where S7's winner goes), 7 (fake client `"lanes"` script block, not a flag), 8 (as `lane_*` counters), 9 for the host side (`lane_v1_host`, `_slow`, `_silent`).
- Item 4 simplified for v1: parties are copies of the host's, so every machine's own initiative gives the same order; the client only follows the host's `next` when they differ (`lane_actor_mismatch`). The fixed lane cycle comes with Phase 4's own parties.
- Open: item 6 (defeat/spectate), the client-side scenario (needs a fake host that merges), the last-enemy merge kill (S1), F2's dead picked actor (counted as `lane_dead_actor_picked`, fixed by S7), damage-over-time on enemies during enemy turns counted once per player.
- Merge rule: `native/protocol/lane_merge.cpp`, unit-tested in CI (`tools/tests/test_lane.cpp`).

**Exit (brief):** 10 consecutive normal fights with no persistent enemy-hash mismatch; fight length measured against solo. Then one real 2-PC session.

### Phase 4: lane combat full

**Party profiles (S2): each player brings their own heroes and builds. Required for the exit (F5).** 3 players (second fake client, then a 3-PC session); status merge and per-player marks (Sciel Foretell); `LaneIntent` preview; teammate strip and damage feed; spectate; rewards per player. Exit: own parties for all 3 players, and an Act 1 boss cleared by 3 players with no unrepaired desync.

### Phase 5: free roam

Written (untested): `native/sync/explore_roam.cpp` and small hooks in `explore.cpp`, `explore_follow.cpp`, `coop.cpp`, `coop_hooks.cpp`. Host option `-e33freeroam=1`, announced by `SessionMode`. See DECISIONS-FORK F7 for every choice and its check. First test: `roam_puppet_host` (one PC, fake client walking circles).

### Status (2026-11-16): all phases written

Phase 3 and 4 code: `coop_lanes.cpp`, `lane_merge.*`, `profile.*`; Phase 5: `explore_roam.cpp`. Choices and their checks: DECISIONS-FORK F7. Nothing has run in the game yet; the order of testing is below.

## Order of work

1. Phase 1 CI green (running now).
2. S7 first: it decides the shape of the sync point. Then S1, S3, S4.
3. Messages and lane mode switch, with protocol round-trip tests. These run in CI without the game.
4. Host merge and sync point, then the fake peer `--lanes`, then scenarios.
5. Ryan runs the scenarios on his PC (each cycle about 3 minutes); I read the logs and fix.
