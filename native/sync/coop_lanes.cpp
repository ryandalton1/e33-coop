// Fork E33Lanes, lane combat v1 (docs/PLAN-LANES.md, Phase 3). Every player plays every hero of their own copy of the
// party locally; the enemies are shared. Each turn, at the earliest hook of the turn advance (StartCharacterTurn pre):
//   client: sends its enemy deltas since the last sync (LaneResult), waits for the host's merged EnemyStateSync, applies it;
//   host:   waits for every client's LaneResult, merges (proto::lane::merge), applies, broadcasts EnemyStateSync.
// The wait is the blocking wait_for for now (frozen frame); lane_hold() is the one place to swap in spike S7's winner.
// Upstream's symmetric replay (ActionCommand, defenses, StateHash/StateSync, draw checks) is off in lane mode.
#include "coop_internal.hpp"

#include "../hooks/statmap.hpp"
#include "../protocol/lane_merge.hpp"

namespace e33::coop::detail {
namespace {

constexpr double kLaneWaitS = 15.0;

bool lane_live() { return s.role != Role::Solo && s.battle.active && s.battle.lanes && !s.battle.ending; }

// Live enemy state, by spot (dead enemies kept, flagged).
std::vector<proto::EnemyState> enemy_snapshot()
{
    std::vector<proto::EnemyState> out;
    for (UObject* e : game::enemies()) {
        const int spot = game::spot(e);
        UObject* st = game::stats(e);
        if (spot < 0 || !st) continue;
        proto::EnemyState x;
        x.spot = static_cast<uint8_t>(spot);
        x.cls = game::char_class(e);
        x.dead = game::is_dead_fast(e);
        x.hp = x.dead ? 0 : ue::get_double(st, STR("CurrentHP"));
        x.max_hp = statmap::max_hp(st);
        x.shield = static_cast<int32_t>(ue::get_int(st, STR("CurrentShieldPoints")));
        x.break_bar = ue::get_int(st, STR("StunBreakBarCurrent"));
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const proto::EnemyState& a, const proto::EnemyState& b) { return a.spot < b.spot; });
    return out;
}

std::vector<proto::HeroBrief> hero_briefs()
{
    std::vector<proto::HeroBrief> out;
    for (UObject* h : game::players()) {
        UObject* st = game::stats(h);
        const int spot = game::spot(h);
        if (!st || spot < 0) continue;
        proto::HeroBrief x;
        x.spot = static_cast<uint8_t>(spot);
        x.cls = game::hardcoded_name(h);
        x.hp = ue::get_double(st, STR("CurrentHP"));
        x.max_hp = statmap::max_hp(st);
        x.ap = ue::get_double(st, STR("CurrentAP"));
        x.alive = !game::is_dead_fast(h);
        out.push_back(std::move(x));
    }
    return out;
}

// Writes an enemy state into the local game (HP, max HP, shield, break, death). Returns the number of failed writes.
int apply_enemies(const std::vector<proto::EnemyState>& want)
{
    int failed = 0, changed = 0;
    for (const auto& w : want) {
        UObject* ch = game::find_unit(false, w.spot);
        UObject* st = game::stats(ch);
        if (!ch || !st) { if (!w.dead) ++failed; continue; }
        const bool dead_here = game::is_dead_fast(ch);
        if (dead_here) continue;                       // never resurrect an enemy (v1): the host's state only goes down
        if (w.max_hp > 0 && std::llround(statmap::max_hp(st)) != std::llround(w.max_hp)) {
            if (!statmap::set_max_hp(st, w.max_hp)) ++failed;
            ++changed;
        }
        if (w.dead) {
            if (!game::kill_unit(ch)) ++failed;
            ++changed;
            continue;
        }
        if (std::llround(ue::get_double(st, STR("CurrentHP"))) != std::llround(w.hp)) {
            ue::Call c(st, STR("SetHP"));                 // updates the health bar (S0); raw write as fallback
            if (c) c.set_double(STR("NewHP"), w.hp).invoke();
            if (std::llround(ue::get_double(st, STR("CurrentHP"))) != std::llround(w.hp)) ue::set_double(st, STR("CurrentHP"), w.hp);
            ++changed;
        }
        if (ue::get_int(st, STR("CurrentShieldPoints")) != w.shield) {
            ue::Call c(st, STR("SetCurrentShieldPoints"));
            if (c) c.set_int(STR("NewValue"), w.shield).set_string(STR("DebugReason"), STR("E33Lanes merge")).invoke();
            else ue::set_int(st, STR("CurrentShieldPoints"), w.shield);
            ++changed;
        }
        if (ue::get_int(st, STR("StunBreakBarCurrent")) != w.break_bar) {
            ue::Call c(st, STR("SetCurrentBreakBarDamage"));
            if (c) c.set_int(STR("NewValue"), w.break_bar).set_object(STR("SourceCharacter"), nullptr).set_string(STR("Reason"), STR("E33Lanes merge")).invoke();
            else ue::set_int(st, STR("StunBreakBarCurrent"), w.break_bar);
            ++changed;
        }
    }
    if (changed) bump("lane_enemy_writes", changed);
    if (failed) bump("lane_apply_failures", failed);
    return failed;
}

// The hold before the next actor's turn. v1: blocking wait (frozen frame), as upstream's turn_actor wait.
template <class Cond>
bool lane_hold(const char* site, Cond cond) { return wait_for(site, cond, kLaneWaitS); }

std::string units_json(const std::vector<proto::EnemyState>& v)
{
    std::string j = "[";
    for (auto& e : v) {
        if (j.size() > 1) j += ",";
        j += JsonObj().add("spot", static_cast<int>(e.spot)).add("hp", e.hp).add("max", e.max_hp).add("shield", e.shield)
                 .add("break", e.break_bar).add("dead", e.dead).str();
    }
    return j + "]";
}

} // namespace

bool lanes_on() { return s.battle.lanes; }

// Host, battle creation: lane settings for BattleStart. hp_mult: -e33lanehp, else one share per player (F4).
void lane_battle_settings(proto::BattleStart& bs)
{
    if (!s.lanes) return;
    int players = 1;
    for (auto& p : s.peers) if (in_game(p)) ++players;
    double mult = s.lane_hp > 0 ? s.lane_hp : static_cast<double>(players);
    mult = std::clamp(mult, 1.0, 8.0);
    bs.lanes = 1;
    bs.hp_mult = mult;
    s.battle.lanes = true;
    s.battle.hp_mult = mult;
    log().write("lane_battle", JsonObj().add("battle", s.battle.id).add("players", players).add("hp_mult", mult));
}

// Turn 1 (host and clients, OnTurnStart): enemy HP scaled, first base taken. Later turns: new base after the sync.
void lane_turn_begin()
{
    auto& b = s.battle;
    if (!b.lanes) return;
    if (b.turn == 1 && !b.lane_scaled) {
        b.lane_scaled = true;
        int n = 0;
        for (UObject* e : game::enemies()) {
            UObject* st = game::stats(e);
            if (!st || game::is_dead_fast(e)) continue;
            const double max = statmap::max_hp(st);
            const double hp = ue::get_double(st, STR("CurrentHP"));
            if (max <= 0 || !statmap::set_max_hp(st, max * b.hp_mult)) { bump("lane_scale_failures"); continue; }
            ue::Call c(st, STR("SetHP"));
            if (c) c.set_double(STR("NewHP"), hp * b.hp_mult).invoke();
            if (std::llround(ue::get_double(st, STR("CurrentHP"))) != std::llround(hp * b.hp_mult)) ue::set_double(st, STR("CurrentHP"), hp * b.hp_mult);
            ++n;
        }
        bump("lane_enemies_scaled", n);
        log().write("lane_scaled", JsonObj().add("mult", b.hp_mult).add("enemies", n).raw("enemies_after", units_json(enemy_snapshot())), b.turn);
    }
    // Base of the turn: the state every machine agrees on (after the sync and the turn-start effects, counted once).
    b.lane_base = enemy_snapshot();
    // Old results and syncs: a few turns are enough.
    for (auto it = b.lane_results.begin(); it != b.lane_results.end();) it = it->first + 4 < b.turn ? b.lane_results.erase(it) : std::next(it);
    for (auto it = b.lane_syncs.begin(); it != b.lane_syncs.end();) it = it->first + 4 < b.turn ? b.lane_syncs.erase(it) : std::next(it);
}

// Messages: replay traffic dropped in lane mode; lane messages stored. True if handled.
bool lane_handle(Peer& p, proto::Message& m)
{
    auto& b = s.battle;
    if (auto* r = std::get_if<proto::LaneResult>(&m)) {
        if (s.role == Role::Host && r->battle_id == b.id && r->player == p.player_id) {
            b.lane_results[r->turn][r->player] = *r;
            b.acks[r->turn].insert(p.player_id);       // proof the player is in this turn (no StateHash in lane mode)
            if (r->turn >= b.turn) b.unresponsive.erase(p.player_id);
        } else {
            bump("lane_results_ignored");
        }
        return true;
    }
    if (auto* y = std::get_if<proto::EnemyStateSync>(&m)) {
        if (s.role == Role::Client && y->battle_id == b.id) b.lane_syncs[y->turn] = *y;
        return true;
    }
    if (auto* h = std::get_if<proto::HeroStatus>(&m)) {
        if (h->battle_id == b.id) b.lane_heroes[h->player] = *h;
        if (s.role == Role::Host) publish(m, &p);       // teammates see each other's party
        return true;
    }
    if (!b.lanes) return false;
    // Upstream replay of other players' heroes: not used in lane mode (every hero here is ours).
    if (std::holds_alternative<proto::ActionCommand>(m) || std::holds_alternative<proto::ComboResult>(m) ||
        std::holds_alternative<proto::FreeAimEvent>(m) || std::holds_alternative<proto::DefenseResult>(m) ||
        std::holds_alternative<proto::DefenseRequest>(m) || std::holds_alternative<proto::StateHash>(m) ||
        std::holds_alternative<proto::StateSync>(m) || std::holds_alternative<proto::Draws>(m) ||
        std::holds_alternative<proto::EnemyActionDone>(m)) {
        bump("lane_replay_dropped_rx");
        return true;
    }
    return false;
}

// Outgoing filter (send_to): the same replay traffic is never sent in lane mode. True = drop.
bool lane_drop_tx(const proto::Message& m)
{
    if (!s.battle.lanes || !s.battle.active) return false;
    const bool drop = std::holds_alternative<proto::ActionCommand>(m) || std::holds_alternative<proto::ComboResult>(m) ||
                      std::holds_alternative<proto::FreeAimEvent>(m) || std::holds_alternative<proto::DefenseResult>(m) ||
                      std::holds_alternative<proto::DefenseRequest>(m) || std::holds_alternative<proto::StateHash>(m) ||
                      std::holds_alternative<proto::StateSync>(m) || std::holds_alternative<proto::Draws>(m) ||
                      std::holds_alternative<proto::EnemyActionDone>(m);
    if (drop) bump("lane_replay_dropped_tx");
    return drop;
}

// StartCharacterTurn pre, lane mode. `slot`: the Character param (the game's pick for the next turn).
void lane_sync_point(UObject** slot)
{
    auto& b = s.battle;
    if (!lane_live() || b.turn == 0) return;
    if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
    const uint32_t turn = b.turn;                       // the turn that just ended
    if (b.lane_synced_turn == turn) return;             // StartCharacterTurn seen twice for one turn
    b.lane_synced_turn = turn;
    const auto local_end = enemy_snapshot();
    const proto::Unit picked = slot && *slot ? actor_unit(*slot) : proto::Unit{proto::ActorKind::Hero, proto::kNoSlot};

    if (s.role == Role::Client) {
        proto::LaneResult r{b.id, turn, s.my_player_id, proto::lane::deltas(b.lane_base, local_end)};
        for (auto& p : s.peers) send_to(p, r);
        for (auto& p : s.peers) send_to(p, proto::HeroStatus{b.id, turn, s.my_player_id, hero_briefs()});
        if (!lane_hold("lane_sync", [&] { return b.lane_syncs.count(turn) > 0 || b.host_end >= 0 || !b.active; })) {
            bump("lane_sync_timeouts");
            log().write("lane_sync_timeout", JsonObj().add("battle", b.id), turn);
            return;
        }
        auto it = b.lane_syncs.find(turn);
        if (it == b.lane_syncs.end()) return;           // battle over on the host
        const auto& y = it->second;
        apply_enemies(y.enemies);
        const uint64_t mine = proto::lane::hash(enemy_snapshot());
        if (mine != y.hash) {
            bump("lane_desyncs");
            log().write("lane_desync", JsonObj().raw("local", units_json(enemy_snapshot())).raw("host", units_json(y.enemies)), turn);
        }
        // Next actor: the host's. v1 parties are copies of the host's, so the game's own pick should already match.
        if (slot && y.next.spot != proto::kNoSlot && (picked.kind != y.next.kind || picked.spot != y.next.spot)) {
            UObject* want = game::find_unit(y.next.kind == proto::ActorKind::Hero, y.next.spot);
            if (want && !game::is_dead_fast(want)) { *slot = want; bump("lane_actors_forced"); }
            else bump("lane_actor_force_failures");
            log().write("lane_actor_mismatch", JsonObj().add("local_kind", static_cast<int>(picked.kind)).add("local_spot", static_cast<int>(picked.spot))
                                                   .add("host_kind", static_cast<int>(y.next.kind)).add("host_spot", static_cast<int>(y.next.spot)), turn);
        }
        bump("lane_turns");
        return;
    }

    // Host: every client in the game sends one result for this turn.
    std::set<uint8_t> expected;
    for (auto& p : s.peers) if (in_game(p)) expected.insert(p.player_id);
    auto all_in = [&] {
        auto it = b.lane_results.find(turn);
        for (uint8_t id : expected) if (it == b.lane_results.end() || !it->second.count(id)) return false;
        return true;
    };
    if (!lane_hold("lane_results", all_in)) {
        bump("lane_result_timeouts");
        std::string missing;
        for (uint8_t id : expected) if (!b.lane_results[turn].count(id)) missing += (missing.empty() ? "" : ",") + std::to_string(id);
        log().write("lane_result_timeout", JsonObj().add("missing", missing), turn);   // merged without them
    }
    std::vector<proto::LaneResult> results;
    for (auto& [id, r] : b.lane_results[turn]) results.push_back(r);
    proto::lane::MergeStats ms;
    const auto merged = proto::lane::merge(b.lane_base, local_end, results, ms);
    apply_enemies(merged);
    proto::EnemyStateSync y;
    y.battle_id = b.id;
    y.turn = turn;
    y.cycle = ++b.lane_cycle;
    y.enemies = merged;
    y.hash = proto::lane::hash(merged);
    y.next = picked;
    if (slot && *slot && !game::is_player(*slot) && game::is_dead_fast(*slot)) {
        // F2: the game picked an enemy the merge just killed. Known v1 gap (S7 moves the hold before the pick).
        bump("lane_dead_actor_picked");
        log().write("lane_dead_actor_picked", JsonObj().add("spot", static_cast<int>(picked.spot)), turn);
    }
    broadcast(y);
    broadcast(proto::HeroStatus{b.id, turn, 0, hero_briefs()});
    bump("lane_turns");
    bump("lane_merged_results", ms.results);
    bump("lane_kills", ms.kills);
    if (ms.overkill > 0) bump("lane_overkill", static_cast<int>(std::llround(ms.overkill)));
    log().write("lane_merge", JsonObj().add("results", ms.results).add("kills", ms.kills).add("overkill", ms.overkill)
                                  .raw("merged", units_json(merged)), turn);
}

} // namespace e33::coop::detail
