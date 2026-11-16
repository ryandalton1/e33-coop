// Fork E33Lanes, lane combat (docs/PLAN-LANES.md, Phases 3-4). Every player plays every hero of their own party locally;
// the enemies are shared. Each turn, at the earliest hook of the turn advance (StartCharacterTurn pre):
//   client: sends its enemy deltas since the last sync (LaneResult) and its heroes (HeroStatus), waits for the host's
//           merged EnemyStateSync, applies it, follows the host's next actor (its own hero in that slot);
//   host:   waits for every live client's LaneResult, merges (proto::lane::merge, statuses and per-player marks
//           included), applies, broadcasts EnemyStateSync.
// The wait never freezes the game: the next turn is held at its start with the game's own turn-start dependency
// (BM.TurnStartNotifyDependencies, upstream D28: the game keeps animating), and the merge runs in the tick.
// Wiped teams stay in the fight as spectators (IsAFullTeamDead answered "no" while another team is standing); the fight
// is lost only when every team is down. Turn order: clients copy the host's hero speeds per slot (same initiative).
// Upstream's symmetric replay (actions, defenses, menus, StateHash/StateSync, draw checks) is off in lane mode.
// Display: teammate strip (status line), teammates' damage feed (toast), teammates' intents (IntentPreview, the brief's
// LaneIntent). Defeat: a wiped client spectates (its local fight ends, it chooses alone); the host stops waiting for it.
#include "coop_internal.hpp"

#include "../hooks/statmap.hpp"
#include "../protocol/lane_merge.hpp"
#include "profile.hpp"

namespace e33::coop::detail {
namespace {

constexpr double kLaneWaitS = 30.0;          // a silent player is merged without after this (the others keep animating)
constexpr double kBannerAfterS = 0.4;

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
        x.speed = statmap::get(st, STR("CharacterCurrentStats"), statmap::kSpeed).value_or(0);
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

void lane_host_heroes(const proto::HeroStatus& h);
namespace { void lane_release(const char* why); }

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
    // Base of the turn: the state every machine agrees on. Normally set when the merged state is applied (the turn starts
    // held); here only when nothing is pending (turn 1, or a turn the game did not hold).
    if (!b.lane_pending) b.lane_base = enemy_snapshot();
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
            lane_host_heroes(*h);
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

// StartBattleEndFlow, lane mode. Normally a wiped team stays in the fight (lane_full_team_dead). If the game ended our
// fight in defeat anyway while the host fights on (the "no" did not reach it), fallback: we leave the fight, the choice on
// the defeat screen is ours, and the host stops waiting for us.
void lane_on_battle_end(int result)
{
    auto& b = s.battle;
    if (!b.lanes) return;
    if (b.lane_pending || b.lane_dep_active) lane_release("battle end");
    if (s.role == Role::Client && result == game::EndDefeat && b.host_end < 0) {
        b.lane_wiped.insert(s.my_player_id);
        send_hero_status(b.turn);
        s.defeat.local_allowed = true;
        bump("lane_wipe_suppress_missed");
        ui::toast("lane_you_wiped", {}, 8.0);
        log().write("lane_left_fight", JsonObj().add("battle", b.id), b.turn);
    }
    if (s.role != Role::Solo) profile::on_battle_end(result);
}

// --- Turn hold (never freezes) ---------------------------------------------------------------------------------------

namespace {

UObject* lane_deps() { return ue::get_object(game::battle_manager(), STR("TurnStartNotifyDependencies")); }

bool local_wiped()
{
    bool any = false;
    for (UObject* h : game::players()) { any = true; if (!game::is_dead_fast(h)) return false; }
    return any;
}

bool any_enemy_alive()
{
    for (UObject* e : game::enemies()) if (!game::is_dead_fast(e)) return true;
    return false;
}

// Host: every team down (ours and every client in the game)?
bool everyone_wiped()
{
    if (!s.battle.lane_spectating && !local_wiped()) return false;
    for (auto& p : s.peers) if (in_game(p) && !s.battle.lane_wiped.count(p.player_id)) return false;
    return true;
}

// Registers our hold on the game's turn-start dependency counter (the game waits for it while animating).
void lane_register(UObject* deps, const char* how)
{
    auto& b = s.battle;
    ue::Call reg(deps, STR("Register Dependency"));
    if (!reg) { bump("lane_hold_register_failures"); return; }
    reg.set_object(STR("Dependency"), game::save_manager()).invoke();
    b.lane_dep_active = true;
    b.lane_dep_since_reset = true;
    bump("lane_holds_registered");
    log().write("lane_hold", JsonObj().add("how", how).add("pending", b.lane_pending).add("spectating", b.lane_spectating), b.turn);
}

void lane_release(const char* why)
{
    auto& b = s.battle;
    // State cleared BEFORE solving: SolveDependency runs the game on synchronously, which Resets the turn-start counter
    // (seen in game 2026-11-16): with lane_pending still set, lane_on_deps re-registered a hold nobody released and the
    // next actor (an enemy) stood still forever.
    const uint32_t pending = b.lane_pending;
    const bool had_dep = b.lane_dep_active;
    b.lane_pending = 0;
    b.lane_dep_active = false;
    if (pending) {
        const int ms = static_cast<int>((s.now - b.lane_pending_since) * 1000.0);
        bump_max("lane_wait_max_ms", ms);
        log().write("lane_released", JsonObj().add("turn", pending).add("why", why).add("waited_ms", ms).add("held", had_dep), b.turn);
    }
    ui::clear_status("lane_wait");
    if (had_dep) {
        b.lane_releasing = true;
        ue::Call solve(lane_deps(), STR("SolveDependency"));
        if (solve) solve.set_object(STR("Dependency"), game::save_manager()).invoke();
        b.lane_releasing = false;
    } else if (pending) {
        bump("lane_holds_missed");             // the game never listened: the turn went on; the state is applied late
    }
}

// After a merge applied here: the held actor may be an enemy the merge killed (it would act invisible, S0): end its turn.
void end_turn_if_dead_actor()
{
    UObject* cur = game::current_character();
    if (!cur || game::is_player(cur) || !game::is_dead_fast(cur)) return;
    ue::Call c(game::battle_manager(), STR("End Character Turn"));
    const bool ok = c && c.invoke();
    bump(ok ? "lane_dead_actor_turns_ended" : "lane_dead_actor_end_failures");
    log().write("lane_dead_actor", JsonObj().add("cls", game::char_class(cur)).add("ended", ok), s.battle.turn);
}

// Client: our heroes' speeds = the host's, slot by slot (same initiative everywhere, so the same next actor).
void align_speeds(const proto::HeroStatus& host)
{
    int n = 0;
    for (const auto& hb : host.heroes) {
        if (hb.speed <= 0) continue;
        UObject* h = game::find_unit(true, hb.spot);
        UObject* st = game::stats(h);
        if (!st) continue;
        const auto mine = statmap::get(st, STR("CharacterCurrentStats"), statmap::kSpeed);
        if (mine && std::llround(*mine) == std::llround(hb.speed)) continue;
        if (statmap::set(st, STR("CharacterCurrentStats"), statmap::kSpeed, hb.speed)) ++n;
        else bump("lane_speed_align_failures");
    }
    if (n) bump("lane_speeds_aligned", n);
}

std::string waiting_names()
{
    auto& b = s.battle;
    if (s.role == Role::Client) return player_name(0);
    std::string w;
    const uint32_t turn = b.lane_pending ? b.lane_pending : b.lane_merged_turn + 1;
    auto it = b.lane_results.find(turn);
    for (auto& p : s.peers) {
        if (!in_game(p) || b.lane_wiped.count(p.player_id)) continue;
        if (it != b.lane_results.end() && it->second.count(p.player_id)) continue;
        w += (w.empty() ? "" : ", ") + player_name(p.player_id);
    }
    return w;
}

// Host: merge turn `turn` (results in, or time out) against our current state, apply, broadcast, release our hold.
void host_merge(uint32_t turn, bool timed_out)
{
    auto& b = s.battle;
    if (timed_out) {
        bump("lane_result_timeouts");
        log().write("lane_result_timeout", JsonObj().add("missing", waiting_names()), turn);
    }
    std::vector<proto::LaneResult> results;
    for (auto& [id, r] : b.lane_results[turn]) results.push_back(r);
    proto::lane::MergeStats ms;
    const auto local_now = enemy_snapshot();
    const auto merged = proto::lane::merge(b.lane_base, local_now, results, ms, b.lane_statuses ? &b.lane_marks : nullptr);
    apply_enemies(merged);
    proto::EnemyStateSync y;
    y.battle_id = b.id;
    y.turn = turn;
    y.cycle = ++b.lane_cycle;
    y.enemies = merged;
    y.hash = proto::lane::hash(merged);
    if (UObject* cur = game::current_character()) y.next = actor_unit(cur);
    broadcast(y);
    if (!b.lane_spectating) send_hero_status(turn);
    damage_feed(b.lane_base, b.lane_local_end, merged);
    b.lane_base = enemy_snapshot();
    b.lane_merged_turn = turn;
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
    if (every_enemy_dead(merged)) { lane_release("victory"); check_battle_end("merge"); return; }
    if (!b.lane_spectating) { lane_release("merged"); end_turn_if_dead_actor(); }
}

} // namespace

// StartCharacterTurn pre, lane mode: the turn that just ended is reported; the next turn will be held at its start
// (dependency, game animating) until the merged state is here. No wait in this hook.
void lane_sync_point(UObject** slot)
{
    (void)slot;
    auto& b = s.battle;
    if (!lane_live() || b.turn == 0 || b.lane_spectating) return;
    if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
    const uint32_t turn = b.turn;                       // the turn that just ended
    if (b.lane_synced_turn == turn) return;             // StartCharacterTurn seen twice for one turn
    b.lane_synced_turn = turn;
    b.lane_local_end = enemy_snapshot();
    if (s.role == Role::Client) {
        proto::LaneResult r{b.id, turn, s.my_player_id, proto::lane::deltas(b.lane_base, b.lane_local_end)};
        for (auto& p : s.peers) send_to(p, r);
        send_hero_status(turn);
    }
    b.lane_pending = turn;
    b.lane_pending_since = s.now;
    b.lane_dep_since_reset = false;
}

// BP_DependencyRefCounter Reset (post) / StartListenToDependencies (pre) on the turn-start counter: hold while pending.
// Reset clears registrations, so ours is redone after each one (upstream D28 notes).
void lane_on_deps(UObject* counter, bool is_reset)
{
    auto& b = s.battle;
    if (!b.lanes || !b.active || b.ending || b.lane_releasing || counter != lane_deps()) return;
    if (!b.lane_pending && !b.lane_spectating) return;
    if (is_reset && b.lane_dep_active) bump("lane_hold_reregistered");   // a Reset dropped ours: registered again below
    if (ue::get_bool(game::battle_manager(), STR("HasActiveCounterAttack"))) return;
    if (is_reset) { lane_register(counter, "reset"); return; }
    if (!b.lane_dep_since_reset) lane_register(counter, "listen");
}

// IsAFullTeamDead (post): our heroes are all down but an enemy stands and another team still fights -> "no": we stay in
// the fight as a spectator. The last team down lets the game see it (defeat for everyone, host's D33 flow).
void lane_full_team_dead(bool& result)
{
    auto& b = s.battle;
    if (!b.lanes || !b.active || b.ending || !result) return;
    if (!local_wiped() || !any_enemy_alive()) return;    // enemies all dead: victory, untouched
    if (s.role == Role::Host) {
        bool other_alive = false;
        for (auto& p : s.peers) if (in_game(p) && !b.lane_wiped.count(p.player_id)) other_alive = true;
        if (!other_alive) return;                        // we are the last team standing... and down: defeat
    }
    result = false;
    if (!b.lane_spectating) {
        b.lane_spectating = true;
        b.lane_wiped.insert(s.my_player_id);
        send_hero_status(b.turn);                        // the others see us down (and stop waiting for us)
        bump("lane_spectates");
        ui::toast("lane_you_wiped_watch", {}, 8.0);
        log().write("lane_spectate", JsonObj().add("battle", b.id), b.turn);
    }
}

// Every frame (game thread).
void lane_tick()
{
    auto& b = s.battle;
    if (!b.lanes || !b.active || b.ending) return;
    // Waiting banner (the game animates meanwhile).
    if (b.lane_pending && s.now - b.lane_pending_since > kBannerAfterS) {
        const std::string w = waiting_names();
        if (!w.empty()) ui::set_status("lane_wait", "lane_waiting", {w});
    }
    if (s.role == Role::Client) {
        uint32_t newest = 0;
        for (auto& [t, y] : b.lane_syncs) if (t > b.lane_applied_turn) newest = std::max(newest, t);
        if (b.lane_spectating) {                         // spectator: follow the fight's enemy state, stay held
            if (newest) { apply_enemies(b.lane_syncs[newest].enemies); b.lane_applied_turn = newest; }
            return;
        }
        if (b.lane_pending && b.lane_syncs.count(b.lane_pending)) {
            const auto y = b.lane_syncs[b.lane_pending];
            apply_enemies(y.enemies);
            b.lane_applied_turn = y.turn;
            if (proto::lane::hash(enemy_snapshot()) != y.hash) {
                bump("lane_desyncs");
                log().write("lane_desync", JsonObj().raw("local", units_json(enemy_snapshot())).raw("host", units_json(y.enemies)), y.turn);
            }
            damage_feed(b.lane_base, b.lane_local_end, y.enemies);
            b.lane_base = enemy_snapshot();
            UObject* cur = game::current_character();
            if (cur && y.next.spot != proto::kNoSlot) {
                const proto::Unit mine = actor_unit(cur);
                if (mine.kind != y.next.kind || mine.spot != y.next.spot) {
                    bump("lane_actor_mismatches");
                    log().write("lane_actor_mismatch", JsonObj().add("local_kind", static_cast<int>(mine.kind)).add("local_spot", static_cast<int>(mine.spot))
                                                           .add("host_kind", static_cast<int>(y.next.kind)).add("host_spot", static_cast<int>(y.next.spot)), y.turn);
                }
            }
            if (every_enemy_dead(y.enemies)) { lane_release("victory"); check_battle_end("merge"); return; }
            lane_release("synced");
            end_turn_if_dead_actor();
            bump("lane_turns");
        } else if (b.lane_pending && (s.now - b.lane_pending_since > kLaneWaitS || b.host_end >= 0)) {
            bump("lane_sync_timeouts");
            lane_release(b.host_end >= 0 ? "host ended" : "timeout");
        }
        return;
    }
    // Host. The turn to merge: ours (pending) or, while we spectate, the clients' next one.
    const uint32_t turn = b.lane_pending ? b.lane_pending : b.lane_spectating ? b.lane_merged_turn + 1 : 0;
    if (!turn) return;
    auto it = b.lane_results.find(turn);
    bool all = true;
    const bool any = it != b.lane_results.end() && !it->second.empty();
    for (auto& p : s.peers) {
        if (!in_game(p) || b.lane_wiped.count(p.player_id)) continue;
        if (it == b.lane_results.end() || !it->second.count(p.player_id)) all = false;
    }
    if (b.lane_spectating && !b.lane_pending) {
        if (everyone_wiped()) {                         // every team down: let the game see our wipe (defeat, D33)
            if (!b.lane_defeat_checked) { b.lane_defeat_checked = true; check_battle_end("all teams down"); }
            return;
        }
        if (!any) return;                               // spectating: the clients' turn starts with their first result
        if (b.lane_spectate_wait_turn != turn) { b.lane_spectate_wait_turn = turn; b.lane_spectate_since = s.now; }
        if (all || s.now - b.lane_spectate_since > kLaneWaitS) host_merge(turn, !all);
        return;
    }
    if (all) host_merge(turn, false);
    else if (s.now - b.lane_pending_since > kLaneWaitS) host_merge(turn, true);
}

// Client: the host's HeroStatus carries its speeds (turn order).
void lane_host_heroes(const proto::HeroStatus& h)
{
    if (s.role == Role::Client && h.player == 0 && s.battle.lanes && s.battle.active) align_speeds(h);
}

} // namespace e33::coop::detail
