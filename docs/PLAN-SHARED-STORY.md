# Shared story: implementation plan

Status: **approved by Ryan, 2026-11-16** ("sure proceed, do all steps"). Replaces the free-roam story rules of DECISIONS-FORK F9. Code references are from `fork/main` at `b995b59`.

## Why

The second playtest (2026-11-16, Lumière, Ryan as client) broke at the Mime:

1. Ryan talked to the Mime. The conversation started a fight **on his PC only**: free roam only routes fights that start from walking into a world enemy (`coop_hooks.cpp:30`), not fights started by a conversation.
2. His friend then talked to the Mime too and started his own fight. The host's turn order was applied to Ryan's private fight: `coop_actor_forced` 16 times, so the Mime got every turn.
3. Ryan joined the host's fight 160 s late. His game then ended it alone (`coop_battle_left`), and the host waited forever.

The root cause is the free-roam model itself. Each PC runs the story on its own (conversations, scripted fights, triggers), and the mod tries to reconcile afterwards. Every new kind of interaction is a new way to drift apart.

Upstream never had this problem. It keeps everyone together: one leader, followers ride along, the leader's interactions and choices are replayed on every PC, and scenes play everywhere at once (upstream D12, D20, D24, D34, D35). The fork's free roam switched each of those safeguards off.

## The principle

**Everyone plays in the host's world** (Ryan, 2026-11-16: "host's world tracks everything and its just like people get to run around and start interactions"). Clients are extra characters walking around in it and starting interactions. Each PC still runs its own copy of the game, but the host's copy is the truth: whatever changes the world (story, items, chests, flags, gold, beaten enemies) is applied on every copy, and where a copy disagrees, the host's wins (in-place repair). The rules below follow from that.

## The rules

1. **One level for everyone.** Whoever changes level takes the whole party along. There are no players in another level, so no fights by encounter name for absent players, and no cross-level travel.
2. **Free walking inside the level.** Each player moves their own character; the others see a puppet (unchanged).
3. **Every story interaction is a party event.** When anyone starts one (talking to an NPC, a trigger zone, a cutscene, a document), that player becomes the **scene leader**:
   - everyone is teleported next to them first;
   - the interaction runs on every PC (upstream replay: `WorldInteract`, then `SceneStart` for what it triggers);
   - the leader advances the lines and picks the answers (`DialogueChoice` replay); everyone else watches with their inputs locked;
   - the leader's skip skips for everyone (upstream `SceneSkip`);
   - the scene ends for everyone together (upstream narrative barrier).
4. **One party event at a time.** While one plays, nobody else can start another ("Party is busy"). The host arbitrates simultaneous starts.
5. **Items and chests: picked up by anyone, separately.** No gather and no teleport (nothing to watch). The pickup or chest is gone for everyone, and the item goes into the shared party inventory (the host's), like gold.
6. **Fights only through the host.** A fight that starts on a client without the host's `BattleStart` is a request to the host, never a fight alone. A fight started from a shared conversation starts on every PC at once (upstream "already engaged", D24).
7. **No teleport back** after a scene or a fight. Everyone stays where it happened.

## What changes in the code

### Scene leader (new, small)

- `s.pilot` becomes the scene leader. It's free (nobody) outside a party event; `is_pilot()` stays true for everyone outside an event (free walking), true only for the leader during one.
- Start: the `OnInteractionRequested` pre-hook (`explore_hooks.cpp`) on a player with no event in progress → that player publishes `WorldInteract` and asks to lead (`PilotRequest`). The host grants the first request (`PilotChange` to everyone) and refuses later ones until the event ends.
- The leader's own interaction is **not delayed**: it runs at once on the leader's PC (no input lag). Others replay it after the gather.
- **Simultaneous starts** (both within the network round-trip): the host's grant decides. The loser's local conversation is closed (`EndDialogue`), and the loser is pulled into the winner's event like everyone else. Logged as `story_event_conflicts`; expected to be rare.
- End: when the barrier releases (every PC finished the scene), the host clears the leader (`PilotChange` to nobody).
- Trigger zones and cutscenes without an interaction already go through `SceneStart`; the player whose game triggered it becomes the leader the same way.

### Gather (new, small)

- On `PilotChange` to a leader, every other PC teleports its pawn beside the leader (`travel_to`, same level only), then replays.
- If a follower's pawn isn't ready (loading, menu), the event waits for it; the existing barrier and "waiting for X" line cover this.

### Turning upstream back on (mostly deleting fork branches)

| Fork branch today | Change |
| --- | --- |
| `explore_replay.cpp:41` replays only chests and pickups (`roam_shared_interaction`) | Replay every interaction of the scene leader, as upstream does |
| `explore.cpp:337` drops `DialogueChoice` in roam | Accept the leader's choices |
| `explore_follow.cpp:552`: follower input lock off in roam | On for non-leaders during an event (upstream lock), off otherwise |
| `explore_narrative.cpp:233`, `explore.cpp:394-411`: anyone's `SceneStart` / `SceneSkip` accepted, a skip only skips your own screen | Only the leader's; the leader's skip skips for all |
| `explore_hooks.cpp:229`, `explore.cpp:348`: only the host's level changes count | Anyone's level change takes the party (host relays, first one wins) |
| `explore.cpp:452`: no snapshot resync in roam | Kept off (it would teleport everyone); in-place repair stays |
| `coop.cpp:578`: fights by encounter name for absent players | **Kept**: the host still needs it for a client's dialogue fight (no world enemy to find) |
| `roam_line_*` (DialogueLine text toasts) | Removed: everyone watches the scene itself |
| Ctrl+F6 cross-level travel | Same level only |

### Fights

- Keep the client's battle request (D34) for world-enemy fights, and extend it: **any** client fight start with no host `BattleStart` within 2 s sends a request; if the host refuses (busy, or the enemy isn't found), the client's local fight is ended at once (`ForceBattleEnd`, neutral) with a toast, instead of running alone.
- Guard against the Mime's endless turns regardless: the client applies the host's actor order only when the host's battle id and encounter match its own fight (`coop_actor_forced` was applied to a different fight).
- World enemies stay active for everyone, since everyone walks freely; any engagement goes to the host as today and pulls everyone into lane combat.

### Kept as they are

Lane combat; puppets and follower hiding; host-relayed flags, gold and inventory repair; beaten enemies removed for everyone.

## Questions for Ryan (defaults applied until answered)

| Question | Default |
| --- | --- |
| Merchants and shops: party event (everyone sees the leader shop, upstream mirror), or local? | Party event, as upstream; revisit if annoying |
| Skip: leader's skip skips for all, or a vote? | Leader skips for all (upstream) |
| Rest points / Expedition Flags | Party event (they change the world: respawns) |

## Order of work and checks

1. **Fight safety first** (fixes the Mime bug even before the rest): the client request for every fight start, the client fight ended when refused, the battle-id guard. Counters: `battle_requests_sent`, `client_fights_ended_refused`, `actor_forced_other_battle`.
2. **One level for everyone**: anyone's level change takes the party. Check: `map_follows` for both directions, no `roam_*` cross-level logs.
3. **Scene leader + gather + upstream replay switched back on.** Checks: `story_events`, `story_event_conflicts`, `scene_syncs_ok` vs `_failed`, `coop_world_hash_diff` after events (should stay at 0).
4. **Cleanup**: remove encounter-name fights, dialogue-line toasts, cross-level travel.
5. **Two-PC session checklist** (Lumière prologue, both directions as host):
   - Mime: talk, fight, both in the same fight from turn 1, normal turns;
   - a story conversation started by the client: both gathered, the client picks, the host watches;
   - both pressing interact on different NPCs at once: one event, the other player pulled in;
   - a cutscene triggered by walking into a zone: both watch, the leader's skip skips for both;
   - level change by the client: both move;
   - a chest opened by one player: gone for both.

Each step ships as its own build on `fork/main`, so a broken step is easy to find.

## Implementation notes (2026-11-16)

- The scene leader is a separate field (`story_leader`, message `StoryEvent`), not `s.pilot`: requests reuse `PilotRequest`, the host answers with `StoryEvent` (to everyone when granted, to the requester when refused). A client leader ends its event by sending `StoryEvent{leader=0xFF}` once nothing is playing anywhere for 2.5 s; the host clears it if the leader leaves, or after 15 min.
- Chests and pickups replay for everyone without an event; climbing holds, ropes, grapple points and enemies never replay.
- A client fight with no host `BattleStart` (`coop_lone_fight`) skips the host's actor order, asks the host after 2 s, is adopted if the host's fight has the same encounter within 10 s, and is ended (neutral) otherwise, or after 15 s without one.
