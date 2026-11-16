// Fork E33Lanes, lane combat (docs/PLAN-LANES.md, Phases 3-4). Every player plays every hero of their own party locally;
// the enemies are shared. Each turn, at the earliest hook of the turn advance (StartCharacterTurn pre):
//   client: sends its enemy deltas since the last sync (LaneResult) and its heroes (HeroStatus), waits for the host's
//           merged EnemyStateSync, applies it, follows the host's next actor (its own hero in that slot);
//   host:   waits for every live client's LaneResult, merges (proto::lane::merge, statuses and per-player marks
//           included), applies, broadcasts EnemyStateSync.
// The wait is the blocking wait_for for now (frozen frame); lane_hold() is the one place to swap in spike S7's winner.
// Upstream's symmetric replay (actions, defenses, menus, StateHash/StateSync, draw checks) is off in lane mode.
// Display: teammate strip (status line), teammates' damage feed (toast), teammates' intents (IntentPreview, the brief's
// LaneIntent). Defeat: a wiped client spectates (its local fight ends, it chooses alone); the host stops waiting for it.
#include "coop_internal.hpp"

#include "../hooks/statmap.hpp"
#include "../protocol/lane_merge.hpp"
#include "profile.hpp"

namespace e33::coop::detail {
namespace {

constexpr double kLaneWaitS = 15.0;

bool lane_live() { return s.role != Role::Solo && s.battle.active && s.battle.lanes && !s.battle.ending; }

std::vector<proto::BuffState> enemy_buffs(UObject* st)
{
    std::vector<proto::BuffState> out;
    for (auto& b : game::buffs(st)) {
        if (game::is_helper_buff(b)) continue;
        out.push_back({b.cls, b.stacks, b.turns, b.permanent});
    }
    std::sort(out.begin(), out.end(), [](const proto::BuffState& a, const proto::BuffState& b) {
        return std::tie(a.cls, a.stacks, a.turns, a.permanent) < std::tie(b.cls, b.stacks, b.turns, b.permanent);
    });
    return out;
}

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
        if (!x.dead) x.buffs = enemy_buffs(st);
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
    std::sort(out.begin(), out.end(), [](const proto::HeroBrief& a, const proto::HeroBrief& b) { return a.spot < b.spot; });
    return out;
}

bool all_dead(const std::vector<proto::HeroBrief>& v)
{
    if (v.empty()) return false;
    for (auto& h : v) if (h.alive) return false;
    return true;
}

// Writes an enemy state into the local game (HP, max HP, shield, break, death, then statuses through upstream's repair).
// Returns the number of failed writes.
int apply_enemies(const std::vector<proto::EnemyState>& want)
{
    int failed = 0, changed = 0;
    for (const auto& w : want) {
        UObject* ch = game::find_unit(false, w.spot);
        UObject* st = game::stats(ch);
        if (!ch || !st) { if (!w.dead) ++failed; continue; }
        if (game::is_dead_fast(ch)) continue;          // never resurrect an enemy: the shared state only goes down
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
    // Statuses: upstream's repair (create, restack, remove buff instances) on a copy of the local state whose enemies carry
    // the wanted statuses. Heroes are copied unchanged, so they are never touched.
    if (s.battle.lane_statuses) {
        const proto::StateSync local = sync::snapshot(s.battle.id, s.battle.turn);
        proto::StateSync target = local;
        bool any = false;
        for (auto& u : target.units) {
            if (u.unit.kind != proto::ActorKind::Enemy || u.dead) continue;
            for (const auto& w : want) {
                if (w.spot != u.unit.spot || w.dead) continue;
                if (u.buffs.size() != w.buffs.size() || !std::equal(u.buffs.begin(), u.buffs.end(), w.buffs.begin(), [](const proto::BuffState& a, const proto::BuffState& b) {
                        return a.cls == b.cls && a.stacks == b.stacks && a.permanent == b.permanent;
                    })) { u.buffs = w.buffs; any = true; }
            }
        }
        if (any) {
            const auto r = sync::repair(local, target);
            bump("lane_status_repairs", r.buffs);
            if (r.failed) { failed += r.failed; log().write("lane_status_repair", JsonObj().raw("repair", sync::repair_json(r)), s.battle.turn); }
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
                 .add("break", e.break_bar).add("dead", e.dead).add("buffs", static_cast<int>(e.buffs.size())).str();
    }
    return j + "]";
}

bool every_enemy_dead(const std::vector<proto::EnemyState>& v)
{
    if (v.empty()) return false;
    for (auto& e : v) if (!e.dead) return false;
    return true;
}

// Merge killed the last enemy: the game only checks for victory at its own moments, so ask it now.
void check_battle_end(const char* why)
{
    UObject* bm = game::battle_manager();
    ue::Call c(bm, STR("CheckBattleEnd"));
    const bool ok = c && c.invoke();
    bump(ok ? "lane_battle_end_checks" : "lane_battle_end_check_failures");
    log().write("lane_check_battle_end", JsonObj().add("why", why).add("ok", ok), s.battle.turn);
}

// Next actor fallback: the game picked something that cannot act here (enemy killed by the merge, or our hero in the host's
// slot is down). Heroes: our lowest alive hero. Returns nullptr if nothing can act.
UObject* first_alive_hero()
{
    UObject* best = nullptr;
    for (UObject* h : game::players())
        if (!game::is_dead_fast(h) && (!best || game::spot(h) < game::spot(best))) best = h;
    return best;
}

std::string short_hp(double v)
{
    if (v >= 10000) return std::to_string(static_cast<int64_t>(std::llround(v / 1000.0))) + "k";
    return std::to_string(static_cast<int64_t>(std::llround(v)));
}

// Teammate strip: one status line with every other player's heroes.
void strip_refresh()
{
    auto& b = s.battle;
    std::string line;
    for (auto& [player, hs] : b.lane_heroes) {
        if (player == s.my_player_id) continue;
        std::string part = player_name(player) + ":";
        if (b.lane_wiped.count(player)) part += " (KO)";
        for (auto& h : hs.heroes) part += " " + h.cls + " " + (h.alive ? short_hp(h.hp) + "/" + short_hp(h.max_hp) : std::string("KO"));
        line += (line.empty() ? "" : "  |  ") + part;
    }
    ui::set_status_text("lanes", "lane_strip", line);
}

// Teammates' damage this turn: what the merged state lost beyond our own damage.
void damage_feed(const std::vector<proto::EnemyState>& base, const std::vector<proto::EnemyState>& local_end, const std::vector<proto::EnemyState>& merged)
{
    double total = 0;
    int kills = 0;
    for (auto& m : merged) {
        const proto::EnemyState* b = nullptr;
        const proto::EnemyState* l = nullptr;
        for (auto& x : base) if (x.spot == m.spot) b = &x;
        for (auto& x : local_end) if (x.spot == m.spot) l = &x;
        if (!b || b->dead) continue;
        const double ours = l ? std::max(0.0, b->hp - (l->dead ? 0.0 : l->hp)) : b->hp;
        const double all = std::max(0.0, b->hp - m.hp);
        total += std::max(0.0, all - ours);
        if (m.dead && l && !l->dead) ++kills;
    }
    if (total < 1) return;
    bump("lane_feed_shown");
    ui::toast(kills ? "lane_feed_kill" : "lane_feed", {short_hp(total), std::to_string(kills)}, 3.0);
}

void send_hero_status(uint32_t turn)
{
    auto& b = s.battle;
    proto::HeroStatus hs{b.id, turn, s.my_player_id, hero_briefs()};
    b.lane_heroes[s.my_player_id] = hs;
    if (s.role == Role::Host) broadcast(hs);
    else for (auto& p : s.peers) send_to(p, hs);
}

bool is_replay(const proto::Message& m)
{
    return std::holds_alternative<proto::ActionCommand>(m) || std::holds_alternative<proto::ComboResult>(m) ||
           std::holds_alternative<proto::FreeAimEvent>(m) || std::holds_alternative<proto::DefenseResult>(m) ||
           std::holds_alternative<proto::DefenseRequest>(m) || std::holds_alternative<proto::StateHash>(m) ||
           std::holds_alternative<proto::StateSync>(m) || std::holds_alternative<proto::Draws>(m) ||
           std::holds_alternative<proto::EnemyActionDone>(m) || std::holds_alternative<proto::MenuState>(m) ||
           std::holds_alternative<proto::AimView>(m);
}

// Teammate's intent (IntentPreview from their own lane hero): shown as a status line, never mirrored.
void show_intent(uint8_t player, const proto::IntentPreview& ip)
{
    std::string what = ip.aiming ? ui::text("lane_intent_aim") : !ip.secondary.empty() ? ip.secondary : ip.action_type ? ui::text("lane_intent_attack") : std::string();
    std::string target;
    for (auto& t : ip.targets) {
        if (t.kind != proto::ActorKind::Enemy) continue;
        UObject* e = game::find_unit(false, t.spot);
        target += (target.empty() ? "" : ", ") + (e ? game::char_class(e) : std::to_string(t.spot));
    }
    if (what.empty() && target.empty()) { ui::clear_status("lane_intent"); return; }
    ui::set_status("lane_intent", "lane_intent", {player_name(player), what, target});
    bump("lane_intents_shown");
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
    s.battle.lane_statuses = s.lane_statuses;
    s.battle.lane_marks.owned = s.lane_marks;
    log().write("lane_battle", JsonObj().add("battle", s.battle.id).add("players", players).add("hp_mult", mult));
}

// Host, session: the fork modes, sent to each client when it is accepted (and when they change).
proto::SessionMode session_mode() { return proto::SessionMode{static_cast<uint8_t>(s.lanes ? 1 : 0), static_cast<uint8_t>(s.freeroam ? 1 : 0), s.lane_hp}; }

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
        log().write("lane_scaled", JsonObj().add("mult", b.hp_mult).raw("enemies_after", units_json(enemy_snapshot())), b.turn);
        send_hero_status(0);
        strip_refresh();
    }
    // Base of the turn: the state every machine agrees on (after the sync and the turn-start effects, counted once).
    b.lane_base = enemy_snapshot();
    for (auto it = b.lane_results.begin(); it != b.lane_results.end();) it = it->first + 4 < b.turn ? b.lane_results.erase(it) : std::next(it);
    for (auto it = b.lane_syncs.begin(); it != b.lane_syncs.end();) it = it->first + 4 < b.turn ? b.lane_syncs.erase(it) : std::next(it);
}

// Messages: lane messages stored, intents shown, replay traffic dropped in lane mode. True if handled.
bool lane_handle(Peer& p, proto::Message& m)
{
    auto& b = s.battle;
    if (auto* sm = std::get_if<proto::SessionMode>(&m)) {
        if (s.role == Role::Client) {
            s.lanes = sm->lanes != 0;
            s.freeroam = sm->freeroam != 0;
            s.lane_hp = sm->lane_hp;
            explore::set_roam(s.freeroam);
            log().write("lane_session_mode", JsonObj().raw("msg", proto::to_json(m)));
        }
        return true;
    }
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
        if (s.role == Role::Host && h->player != p.player_id) return true;
        if (h->battle_id == b.id) {
            b.lane_heroes[h->player] = *h;
            if (all_dead(h->heroes) && !b.lane_wiped.count(h->player)) {
                b.lane_wiped.insert(h->player);
                bump("lane_players_wiped");
                ui::toast("lane_player_wiped", {player_name(h->player)}, 5.0);
                log().write("lane_player_wiped", JsonObj().add("player", static_cast<int>(h->player)), b.turn);
            }
            strip_refresh();
        }
        if (s.role == Role::Host) publish(m, &p);       // teammates see each other's party
        return true;
    }
    if (!b.lanes) return false;
    if (auto* ip = std::get_if<proto::IntentPreview>(&m)) {
        if (ip->battle_id != b.id) return true;
        // Host relays clients' intents with spot = 0x80 + player; an untagged one on a client is the host's own.
        const uint8_t from = s.role == Role::Host ? p.player_id : ip->spot >= 0x80 ? static_cast<uint8_t>(ip->spot - 0x80) : 0;
        if (s.role == Role::Host) {
            proto::IntentPreview tagged = *ip;
            tagged.spot = static_cast<uint8_t>(0x80 + p.player_id);   // lane mode: spot carries the sender (0x80 + player)
            publish(tagged, &p);
        }
        if (from != s.my_player_id) show_intent(from, *ip);
        return true;
    }
    if (is_replay(m)) {
        bump("lane_replay_dropped_rx");
        return true;
    }
    return false;
}

// Outgoing filter (send_to): replay traffic never sent in lane mode; our intents tagged with our player id. True = drop.
bool lane_drop_tx(const proto::Message& m)
{
    if (!s.battle.lanes || !s.battle.active) return false;
    if (is_replay(m)) { bump("lane_replay_dropped_tx"); return true; }
    return false;
}

// StartBattleEndFlow, lane mode. Client whose party is down while the host fights on: spectator. Its local fight ends
// (the game's defeat screen); the choice there is its own (no host choice will come for it), and the host stops waiting.
void lane_on_battle_end(int result)
{
    auto& b = s.battle;
    if (!b.lanes) return;
    if (s.role == Role::Client && result == game::EndDefeat && b.host_end < 0) {
        b.lane_wiped.insert(s.my_player_id);
        send_hero_status(b.turn);                       // all heroes down: the host drops us from the wait
        s.defeat.local_allowed = true;
        bump("lane_spectates");
        ui::toast("lane_you_wiped", {}, 8.0);
        log().write("lane_spectate", JsonObj().add("battle", b.id), b.turn);
    }
    if (s.role != Role::Solo) profile::on_battle_end(result);
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
        send_hero_status(turn);
        if (!lane_hold("lane_sync", [&] { return b.lane_syncs.count(turn) > 0 || b.host_end >= 0 || !b.active; })) {
            bump("lane_sync_timeouts");
            log().write("lane_sync_timeout", JsonObj().add("battle", b.id), turn);
            return;
        }
        auto it = b.lane_syncs.find(turn);
        if (it == b.lane_syncs.end()) return;           // battle over on the host
        const auto y = it->second;
        apply_enemies(y.enemies);
        const auto after = enemy_snapshot();
        if (proto::lane::hash(after) != y.hash) {
            bump("lane_desyncs");
            log().write("lane_desync", JsonObj().raw("local", units_json(after)).raw("host", units_json(y.enemies)), turn);
        }
        damage_feed(b.lane_base, local_end, y.enemies);
        if (every_enemy_dead(y.enemies)) { check_battle_end("merge"); return; }
        // Next actor: the host's (lane k = our own hero in slot k; an enemy spot as is).
        if (slot && y.next.spot != proto::kNoSlot && (picked.kind != y.next.kind || picked.spot != y.next.spot || (*slot && game::is_dead_fast(*slot)))) {
            UObject* want = game::find_unit(y.next.kind == proto::ActorKind::Hero, y.next.spot);
            if ((!want || game::is_dead_fast(want)) && y.next.kind == proto::ActorKind::Hero) { want = first_alive_hero(); bump("lane_slot_fallbacks"); }
            if (want && !game::is_dead_fast(want)) { *slot = want; bump("lane_actors_forced"); }
            else bump("lane_actor_force_failures");
            log().write("lane_actor_forced", JsonObj().add("local_kind", static_cast<int>(picked.kind)).add("local_spot", static_cast<int>(picked.spot))
                                                 .add("host_kind", static_cast<int>(y.next.kind)).add("host_spot", static_cast<int>(y.next.spot)), turn);
        }
        bump("lane_turns");
        return;
    }

    // Host: every live client in the game sends one result for this turn (wiped players spectate: not waited for).
    std::set<uint8_t> expected;
    for (auto& p : s.peers) if (in_game(p) && !b.lane_wiped.count(p.player_id)) expected.insert(p.player_id);
    auto all_in = [&] {
        auto it = b.lane_results.find(turn);
        for (uint8_t id : expected) {
            if (b.lane_wiped.count(id)) continue;
            if (it == b.lane_results.end() || !it->second.count(id)) return false;
        }
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
    const auto merged = proto::lane::merge(b.lane_base, local_end, results, ms, b.lane_statuses ? &b.lane_marks : nullptr);
    apply_enemies(merged);
    proto::EnemyStateSync y;
    y.battle_id = b.id;
    y.turn = turn;
    y.cycle = ++b.lane_cycle;
    y.enemies = merged;
    y.hash = proto::lane::hash(merged);
    y.next = picked;
    // F2: the game picked an enemy the merge just killed. It would act invisible (S0): a hero acts instead.
    if (slot && *slot && !game::is_player(*slot) && game::is_dead_fast(*slot)) {
        if (UObject* h = first_alive_hero()) {
            *slot = h;
            y.next = actor_unit(h);
            bump("lane_dead_actor_replaced");
        } else {
            bump("lane_dead_actor_picked");
        }
        log().write("lane_dead_actor_picked", JsonObj().add("spot", static_cast<int>(picked.spot)).add("replaced", y.next.kind == proto::ActorKind::Hero), turn);
    }
    broadcast(y);
    send_hero_status(turn);
    damage_feed(b.lane_base, local_end, merged);
    bump("lane_turns");
    bump("lane_merged_results", ms.results);
    bump("lane_kills", ms.kills);
    if (ms.statuses_added) bump("lane_statuses_added", ms.statuses_added);
    if (ms.statuses_spent) bump("lane_statuses_spent", ms.statuses_spent);
    if (ms.marks_refused) bump("lane_marks_refused", ms.marks_refused);
    if (ms.marks_restored) bump("lane_marks_restored", ms.marks_restored);
    if (ms.overkill > 0) bump("lane_overkill", static_cast<int>(std::llround(ms.overkill)));
    log().write("lane_merge", JsonObj().add("results", ms.results).add("kills", ms.kills).add("overkill", ms.overkill)
                                  .add("statuses_added", ms.statuses_added).add("statuses_spent", ms.statuses_spent)
                                  .raw("merged", units_json(merged)), turn);
    if (every_enemy_dead(merged)) check_battle_end("merge");
}

} // namespace e33::coop::detail
