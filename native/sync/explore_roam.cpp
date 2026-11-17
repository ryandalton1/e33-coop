// Fork E33Lanes, free roam (docs/PLAN-LANES.md Phase 5, brief Feature B). Every player drives their own pawn and may be
// in another zone. Upstream's single pilot is replaced by "everyone pilots":
//  - every player publishes PilotState (position, level, movement mode, pawn class) at the pilot rate; the host relays;
//  - other players are drawn as puppets: a non-colliding actor of the sender's pawn class, interpolated from a ~100 ms
//    buffer (upstream D23's idea), hidden when the sender is in another level;
//  - world state: persistent flags (FlagSet, upstream D38) from any player, applied everywhere; interactions, dialogue
//    choices, map changes and scenes are no longer replayed (each player's own);
//  - battles: any player's engagement asks the host (upstream D34 request, encounter name as fallback when the host is
//    elsewhere); the host's BattleStart pulls everyone in (lane combat), each returns to their own position after;
//  - story cinematics gather the party (teleport to the player who triggered it; the narrative barrier then syncs it);
//    dialogues stay local;
//  - Ctrl+F6: travel to the next player (same level: teleport; other level: change map, then teleport).
// Puppets are spike S5. v1 (glided, teleported each frame) showed a static Gustave sliding (Ryan, 2026-11-16). v2: AI
// controller + movement input (the game's locomotion animates it) and the sender's character (LoadAndActivateCharacter).
#include "explore_internal.hpp"

#include <algorithm>
#include <cmath>
#include <deque>

namespace e33::explore::detail {
namespace {

constexpr double kInterpDelayS = 0.10;
constexpr double kStaleS = 5.0;            // no state for this long: puppet hidden
constexpr double kSpawnRetryS = 5.0;

struct Sample { double t = 0; double pos[3]{}; double yaw = 0; double vel[3]{}; };

struct Puppet {
    UObject* actor = nullptr;
    std::string actor_level;               // level the actor was spawned in (gone with it)
    std::string cls;                       // pawn class it was spawned from
    proto::PilotState last;
    double last_rx = -1;
    double clock_offset = 0;               // local time - sender clock
    std::deque<Sample> buf;
    double spawn_retry_at = 0;
    bool hidden = false;
    int spawn_failures = 0;
    bool walks = false;                    // driven like a character (AI controller, walking, animated); else glided
    std::string chr;                       // character shown (LoadAndActivateCharacter), empty = class default
    double next_chr_try = 0;
};

std::map<uint8_t, Puppet> g_puppets;
const bool g_walk = cmdline::arg(L"e33roamwalk") != L"0";   // -e33roamwalk=0: glided puppets (no AI controller)
std::string g_level;                       // local level seen last frame
bool g_key_down = false;
uint8_t g_travel_last = 0xFF;
struct Travel { uint8_t player = 0xFF; std::string level; double since = -1; } g_travel;

UObject* gameplay_statics() { return ue::find_object(L"/Script/Engine.Default__GameplayStatics"); }

// FTransform (UE5, double): rotation quaternion, translation, scale (each padded to 16 bytes).
struct alignas(16) Transform {
    double q[4];
    double t[3]; double pad0;
    double s[3]; double pad1;
};
static_assert(sizeof(Transform) == 96);

Transform make_transform(const double pos[3], double yaw_deg)
{
    Transform x{};
    const double h = yaw_deg * 3.14159265358979323846 / 360.0;   // half angle, radians
    x.q[0] = 0; x.q[1] = 0; x.q[2] = std::sin(h); x.q[3] = std::cos(h);
    x.t[0] = pos[0]; x.t[1] = pos[1]; x.t[2] = pos[2];
    x.s[0] = x.s[1] = x.s[2] = 1.0;
    return x;
}

// Walking puppet (animated): an AI controller so the movement component runs, walking mode, collision kept for the floor
// but ignoring pawns and the camera, and no overlap events (it must never trigger a battle, a zone or a pickup).
bool make_walker(UObject* actor)
{
    ue::Call ctrl(actor, STR("SpawnDefaultController"));
    if (!ctrl || !ctrl.invoke()) return false;
    if (!ue::get_object(actor, STR("Controller"))) return false;
    if (UObject* cap = ue::get_object(actor, STR("CapsuleComponent"))) {
        for (int ch : {2, 4}) {   // ECC_Pawn, ECC_Camera -> ECR_Ignore
            ue::Call c(cap, STR("SetCollisionResponseToChannel"));
            if (c) c.set_int(STR("Channel"), ch).set_int(STR("NewResponse"), 0).invoke();
        }
        ue::Call o(cap, STR("SetGenerateOverlapEvents"));
        if (o) o.set_bool(STR("bInGenerateOverlapEvents"), false).invoke();
    } else {
        return false;
    }
    if (UObject* mesh = ue::get_object(actor, STR("Mesh"))) {
        ue::Call c(mesh, STR("SetCollisionEnabled"));
        if (c) c.set_int(STR("NewType"), 0).invoke();
        ue::Call o(mesh, STR("SetGenerateOverlapEvents"));
        if (o) o.set_bool(STR("bInGenerateOverlapEvents"), false).invoke();
    }
    game::set_movement_mode(actor, 1);   // walking
    return true;
}

UObject* spawn_puppet(const proto::PilotState& p, std::string& err, bool& walks)
{
    if (p.pawn.empty()) { err = "no pawn class"; return nullptr; }
    auto* cls = ue::find_class_by_short_name(p.pawn);
    if (!cls) { err = "class not loaded: " + p.pawn; return nullptr; }
    UObject* gs = gameplay_statics();
    UObject* ctx = game::explore_pawn();
    if (!gs || !ctx) { err = "no statics or world"; return nullptr; }
    const double pos[3] = {p.x, p.y, p.z};
    const Transform tr = make_transform(pos, p.yaw);
    ue::Call begin(gs, STR("BeginDeferredActorSpawnFromClass"));
    if (!begin) { err = "BeginDeferredActorSpawnFromClass missing"; return nullptr; }
    if (begin.param_size(STR("SpawnTransform")) != sizeof(Transform)) { err = "unexpected FTransform size"; return nullptr; }
    begin.set_object(STR("WorldContextObject"), ctx).set_object(STR("ActorClass"), reinterpret_cast<UObject*>(cls)).set_raw(STR("SpawnTransform"), &tr)
         .set_int(STR("CollisionHandlingOverride"), 1);   // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    if (!begin.invoke()) { err = "begin spawn failed"; return nullptr; }
    UObject* actor = begin.out_object(STR("ReturnValue"));
    if (!actor) { err = "spawn returned null"; return nullptr; }
    ue::Call finish(gs, STR("FinishSpawningActor"));
    if (finish) finish.set_object(STR("Actor"), actor).set_raw(STR("SpawnTransform"), &tr).invoke();
    walks = g_walk && make_walker(actor);
    if (!walks) {
        // Fallback (S5 v1): no collision, no gravity, glided (no walk animation).
        { ue::Call c(actor, STR("SetActorEnableCollision")); if (c) c.set_bool(STR("bNewActorEnableCollision"), false).invoke(); }
        game::set_movement_mode(actor, 5);
    }
    return actor;
}

// Walking puppets move with movement input toward the interpolated pose (the game's locomotion animates them); far off,
// another floor or a glided puppet: placed directly.
void drive_puppet(Puppet& pp, const Sample& x)
{
    double cur[3];
    if (!pp.walks || !game::location(pp.actor, cur)) { game::teleport_pawn(pp.actor, x.pos, x.yaw); return; }
    const double dx = x.pos[0] - cur[0], dy = x.pos[1] - cur[1], dz = x.pos[2] - cur[2];
    const double d = std::sqrt(dx * dx + dy * dy);
    if (d > 400.0 || std::fabs(dz) > 250.0) { game::teleport_pawn(pp.actor, x.pos, x.yaw); bump("roam_puppet_snaps"); return; }
    if (d < 20.0) return;                                        // there: idle animation
    const double speed = std::sqrt(x.vel[0] * x.vel[0] + x.vel[1] * x.vel[1]);
    const double scale = std::clamp(std::max(speed / 600.0, d / 150.0), 0.25, 1.0);
    game::move_pawn(pp.actor, dx / d, dy / d, scale);
}

void destroy_puppet(Puppet& pp, const char* why)
{
    if (pp.actor && pp.actor_level == game::current_level() && !game::map_change_in_progress()) {
        ue::Call c(pp.actor, STR("K2_DestroyActor"));
        if (c) c.invoke();
    }
    if (pp.actor) log().write("roam_puppet_destroyed", JsonObj().add("why", why).add("cls", pp.cls));
    pp.actor = nullptr;
}

void set_hidden(Puppet& pp, bool hide)
{
    if (!pp.actor || pp.hidden == hide) return;
    ue::Call c(pp.actor, STR("SetActorHiddenInGame"));
    if (c) c.set_bool(STR("bNewHidden"), hide).invoke();
    pp.hidden = hide;
}

// Interpolated pose at local time `now` (sender clock mapped to ours, minus the buffer delay).
bool pose_at(Puppet& pp, double now, Sample& out)
{
    if (pp.buf.empty()) return false;
    const double t = now - pp.clock_offset - kInterpDelayS;
    while (pp.buf.size() > 2 && pp.buf[1].t <= t) pp.buf.pop_front();
    const Sample& a = pp.buf.front();
    if (pp.buf.size() == 1 || t <= a.t) { out = a; return true; }
    const Sample& b = pp.buf[1];
    const double span = b.t - a.t;
    const double k = span > 1e-6 ? std::clamp((t - a.t) / span, 0.0, 1.5) : 1.0;   // short extrapolation past b
    for (int i = 0; i < 3; ++i) { out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * k; out.vel[i] = b.vel[i]; }
    double dy = std::fmod(b.yaw - a.yaw + 540.0, 360.0) - 180.0;
    out.yaw = a.yaw + dy * std::min(k, 1.0);
    out.t = t;
    return true;
}

void travel_to(uint8_t player, const char* why)
{
    auto it = g_puppets.find(player);
    if (it == g_puppets.end() || it->second.last_rx < 0) { ui::toast("roam_no_players", {}, 4.0); return; }
    const auto& p = it->second.last;
    UObject* pawn = game::explore_pawn();
    if (!pawn || game::in_battle() || game::map_change_in_progress()) return;
    if (p.level == game::current_level()) {
        const double pos[3] = {p.x + 120.0, p.y, p.z};       // beside them, not inside
        const bool ok = game::teleport_pawn(pawn, pos, p.yaw);
        bump(ok ? "roam_travels" : "roam_travel_failures");
        log().write("roam_travel", JsonObj().add("to", static_cast<int>(player)).add("why", why).add("same_level", true).add("ok", ok));
    } else {
        const bool ok = game::change_map(ue::from_utf8(p.level), L"");
        if (ok) g_travel = Travel{player, p.level, s.now};
        bump(ok ? "roam_travel_maps" : "roam_travel_failures");
        log().write("roam_travel", JsonObj().add("to", static_cast<int>(player)).add("why", why).add("level", p.level).add("ok", ok));
    }
    ui::toast("roam_travel", {s.net.name_of ? s.net.name_of(player) : std::to_string(player)}, 4.0);
}

} // namespace

void roam_push(const proto::PilotState& p)
{
    auto& pp = g_puppets[p.pilot];
    const double sender_t = p.t_ms ? p.t_ms / 1000.0 : s.now;
    if (pp.last_rx < 0 || std::fabs((s.now - sender_t) - pp.clock_offset) > 2.0) { pp.clock_offset = s.now - sender_t; pp.buf.clear(); }
    if (!pp.buf.empty() && sender_t <= pp.buf.back().t) return;   // late or duplicate
    Sample x;
    x.t = sender_t;
    x.pos[0] = p.x; x.pos[1] = p.y; x.pos[2] = p.z;
    x.vel[0] = p.vx; x.vel[1] = p.vy; x.vel[2] = p.vz;
    x.yaw = p.yaw;
    pp.buf.push_back(x);
    while (pp.buf.size() > 64) pp.buf.pop_front();
    pp.last = p;
    pp.last_rx = s.now;
    bump("roam_states_rx");
}

void roam_tick()
{
    const std::string level = game::current_level();
    if (level != g_level || game::map_change_in_progress()) {
        // Local level changed: the puppets were destroyed with it.
        for (auto& [id, pp] : g_puppets) { pp.actor = nullptr; pp.hidden = false; }
        g_level = game::map_change_in_progress() ? std::string() : level;
    }
    const bool can_draw = game::explore_pawn() && !game::in_battle() && !game::map_change_in_progress();
    for (auto& [id, pp] : g_puppets) {
        if (id == s.me) continue;
        const bool here = pp.last_rx >= 0 && pp.last.level == level && s.now - pp.last_rx < kStaleS;
        if (!can_draw) continue;
        if (!here) { set_hidden(pp, true); continue; }
        if (!pp.actor || pp.cls != pp.last.pawn) {
            if (pp.actor) destroy_puppet(pp, "class changed");
            if (s.now < pp.spawn_retry_at) continue;
            std::string err;
            pp.actor = spawn_puppet(pp.last, err, pp.walks);
            pp.chr.clear();
            if (!pp.actor) {
                pp.spawn_retry_at = s.now + kSpawnRetryS;
                if (++pp.spawn_failures <= 5) log().write("roam_puppet_spawn_failed", JsonObj().add("player", static_cast<int>(id)).add("err", err));
                bump("roam_puppet_spawn_failures");
                continue;
            }
            pp.actor_level = level;
            pp.cls = pp.last.pawn;
            pp.hidden = false;
            bump("roam_puppets_spawned");
            bump(pp.walks ? "roam_puppets_walking" : "roam_puppets_gliding");
            log().write("roam_puppet_spawned", JsonObj().add("player", static_cast<int>(id)).add("cls", pp.cls).add("level", level).add("walks", pp.walks));
        }
        set_hidden(pp, false);
        Sample x;
        if (!pose_at(pp, s.now, x)) continue;
        // Shown character = the sender's (same call as upstream's follower, PilotState.chr), one try a second.
        if (!pp.last.chr.empty() && pp.chr != pp.last.chr && s.now >= pp.next_chr_try) {
            pp.next_chr_try = s.now + 1.0;
            ue::Call c(pp.actor, STR("LoadAndActivateCharacter"));
            const bool ok = c && c.set_fname(STR("CharacterName"), ue::from_utf8(pp.last.chr).c_str()).set_bool(STR("HasSwitchCharacter"), true).invoke();
            bump(ok ? "roam_puppet_character_switches" : "roam_puppet_character_errors");
            log().write("roam_puppet_character", JsonObj().add("player", static_cast<int>(id)).add("chr", pp.last.chr).add("ok", ok));
            if (ok) pp.chr = pp.last.chr;
        }
        drive_puppet(pp, x);
    }

    // Pending travel to another level: once there, next to the player.
    if (g_travel.player != 0xFF) {
        if (s.now - g_travel.since > 60.0) g_travel = Travel{};
        else if (level == g_travel.level && game::explore_pawn() && !game::map_change_in_progress()) {
            const uint8_t who = g_travel.player;
            g_travel = Travel{};
            travel_to(who, "arrived");
        }
    }

    // Ctrl+F6: travel to the next player.
    const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (down && !g_key_down) {
        std::vector<uint8_t> ids;
        for (auto& [id, pp] : g_puppets) if (id != s.me && pp.last_rx >= 0 && s.now - pp.last_rx < 30.0) ids.push_back(id);
        if (ids.empty()) ui::toast("roam_no_players", {}, 4.0);
        else {
            auto it = std::upper_bound(ids.begin(), ids.end(), g_travel_last);
            const uint8_t to = it == ids.end() ? ids.front() : *it;
            g_travel_last = to;
            travel_to(to, "key");
        }
    }
    g_key_down = down;
}

// A remote player started a story cinematic: gather (teleport to them) unless busy here.
void roam_gather(uint8_t player)
{
    if (!s.roam_gather || game::in_battle() || game::in_dialogue() || game::playing_cinematic()) return;
    bump("roam_gathers");
    ui::toast("roam_gather", {s.net.name_of ? s.net.name_of(player) : std::to_string(player)}, 5.0);
    travel_to(player, "story");
}

void roam_peer_left(uint8_t player)
{
    auto it = g_puppets.find(player);
    if (it == g_puppets.end()) return;
    destroy_puppet(it->second, "player left");
    g_puppets.erase(it);
}

// A free-roam fight was won (every machine, at its own battle end). Players who were elsewhere fought it by encounter
// name, so their game never marked the world enemy beaten. Probe 2026-11-16 (tools/dev/probe-defeat.ps1): the game
// keeps beaten enemies in the GI (SetBattledEnemy(EnemyID) -> BattledEnemies / TransientBattledEnemies, the latter
// emptied at an Expedition Flag rest) and hides the actor (hidden, no collision, CurrentAiState 9, no tick). We call
// the same SetBattledEnemy (the game then decides persistence and respawn, as solo) and hide the actor if loaded here.
void roam_enemy_defeated(const std::string& actor, const std::string& gid)
{
    bool marked = false, hidden = false;
    if (!gid.empty() && gid != "None") {
        const std::wstring id = ue::from_utf8(gid);
        ue::Call c(game::gi(), STR("SetBattledEnemy"));
        if (c) { c.set_fname(STR("EnemyID"), id.c_str()); marked = c.invoke(); }
    }
    if (UObject* e = actor.empty() ? nullptr : game::find_world_enemy(actor)) {
        ue::Call h(e, STR("SetActorHiddenInGame"));
        if (h) { h.set_bool(STR("bNewHidden"), true); hidden = h.invoke(); }
        ue::Call col(e, STR("SetActorEnableCollision"));
        if (col) col.set_bool(STR("bNewActorEnableCollision"), false).invoke();
        ue::Call tk(e, STR("SetActorTickEnabled"));
        if (tk) tk.set_bool(STR("bEnabled"), false).invoke();
        ue::set_int(e, STR("CurrentAiState"), 9);
    }
    bump(marked ? "roam_enemies_marked" : "roam_enemy_mark_failures");
    if (hidden) bump("roam_enemies_hidden");
    log().write("roam_enemy_defeated", JsonObj().add("actor", actor).add("id", gid).add("marked", marked).add("hidden", hidden));
}

void roam_reset()
{
    for (auto& [id, pp] : g_puppets) destroy_puppet(pp, "reset");
    g_puppets.clear();
    g_travel = Travel{};
}

} // namespace e33::explore::detail
