// Fork E33Lanes, free roam (docs/PLAN-LANES.md Phase 5, brief Feature B). Every player drives their own pawn and may be
// in another zone. Upstream's single pilot is replaced by "everyone pilots":
//  - every player publishes PilotState (position, level, movement mode, pawn class) at the pilot rate; the host relays;
//  - other players are drawn as puppets: a non-colliding actor of the sender's pawn class, interpolated from a ~100 ms
//    buffer (upstream D23's idea), hidden when the sender is in another level;
//  - shared story, modelled on Online Expedition (docs/DECISIONS-FORK.md F9): the host's story is the party's. Flags
//    from any player apply everywhere; the host's map changes take everyone along; cutscenes (and their skip) from any
//    player play for everyone (a skip skips only one's own screen); interactions with world-state objects (chests,
//    pickups) from anyone replay everywhere; every dialogue line (text, speaker, voice) is shown and heard by all;
//    followers are repaired in place from the host (gold, inventory, flags, beaten enemies);
//  - battles: any player's engagement asks the host (upstream D34 request, encounter name as fallback when the host is
//    elsewhere); the host's BattleStart pulls everyone in (lane combat), each returns to their own position after;
//  - a cinematic first gathers the party (teleport to the player who triggered it), then plays everywhere (upstream
//    SceneStart replay; the narrative barrier syncs its end); dialogues themselves stay with their player;
//  - Ctrl+F6: travel to the next player (same level only: everyone shares one level, docs/PLAN-SHARED-STORY.md).
// Puppets are spike S5. v1 (glided, teleported each frame) showed a static Gustave sliding (Ryan, 2026-11-16). v2: AI
// controller + movement input (the game's locomotion animates it) and the sender's character (LoadAndActivateCharacter).
#include "explore_internal.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

#include <Unreal/UObjectGlobals.hpp>

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

// --- AI party followers hidden (Ryan, playtest 2026-11-16: "really glitchy about the ai follower units ... just remove
// them so its just me and i can still switch to the other ones visually"). Every world character that is neither our
// own pawn nor a puppet is the game's AI party follower (a puppet, being a world character too, even got its own Lune
// that followed us). Hidden, no collision, no tick (its AI stops). Our pawn is never touched, so switching characters
// still shows the new one; an actor the game hands us later is restored at once. Cutscenes and conversations: restored
// in case the game shows them there. -e33hidefollowers=0 keeps them.
const bool g_hide_followers = cmdline::arg(L"e33hidefollowers") != L"0";
std::set<UObject*> g_followers_hidden;     // actors of the current level we hid
double g_followers_next = 0;
std::string g_followers_scan_level;   // level whose nearby characters were logged (diagnostic)

void follower_visible(UObject* a, bool show)
{
    { ue::Call c(a, STR("SetActorHiddenInGame")); if (c) c.set_bool(STR("bNewHidden"), !show).invoke(); }
    { ue::Call c(a, STR("SetActorEnableCollision")); if (c) c.set_bool(STR("bNewActorEnableCollision"), show).invoke(); }
    { ue::Call c(a, STR("SetActorTickEnabled")); if (c) c.set_bool(STR("bEnabled"), show).invoke(); }
}

void followers_restore(const char* why)
{
    if (g_followers_hidden.empty()) return;
    for (UObject* a : g_followers_hidden) follower_visible(a, true);
    log().write("roam_followers_restored", JsonObj().add("n", static_cast<int>(g_followers_hidden.size())).add("why", why));
    g_followers_hidden.clear();
}

void followers_tick(bool can_draw)
{
    if (!g_hide_followers) return;
    if (game::playing_cinematic() || game::in_dialogue()) { followers_restore("scene"); return; }
    UObject* pawn = game::explore_pawn();
    if (!can_draw || !pawn) return;
    if (g_followers_hidden.erase(pawn)) {                 // the game gave us one of them (character switch)
        follower_visible(pawn, true);
        bump("roam_followers_became_pawn");
    }
    if (s.now < g_followers_next) return;
    g_followers_next = s.now + 1.0;
    std::set<UObject*> puppets;
    for (auto& [id, pp] : g_puppets) if (pp.actor) puppets.insert(pp.actor);
    // Playtest 2026-11-16 (0.7.1): the party followers are not BP_jRPG_Character_World_C (nothing found). Every
    // ACharacter is looked at (FindAllOf includes subclasses: it finds every world enemy by their base class). A
    // follower: near us, not an NPC, an enemy or a world character (pawn, puppets, Esquie), and either a party member
    // (battle stats with a hardcoded name) or a class named like a follower.
    double me[3]{};
    game::location(pawn, me);
    std::vector<UObject*> all;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("Character"), all);
    const std::string level = game::current_level();
    const bool scan_log = g_followers_scan_level != level;   // diagnostic once per level: every character near us
    std::string near_list;
    int hidden = 0;
    std::string names;
    for (UObject* a : all) {
        if (!a || a == pawn || puppets.count(a) || g_followers_hidden.count(a)) continue;
        const std::string full = ue::full_name(a);
        if (full.find("Default__") != std::string::npos || full.find(":PersistentLevel.") == std::string::npos) continue;
        double at[3]{};
        if (!game::location(a, at)) continue;
        const double dist = std::hypot(at[0] - me[0], at[1] - me[1], at[2] - me[2]);
        if (dist > 3000.0) continue;
        const std::string cls = ue::short_name(a->GetClassPrivate());
        const std::string chr = game::hardcoded_name(a);
        if (scan_log && near_list.size() < 3000) {
            std::string chain;
            int depth = 0;
            for (RC::Unreal::UStruct* c = a->GetClassPrivate(); c && depth < 5; c = c->GetSuperStruct(), ++depth)
                chain += (chain.empty() ? "" : ">") + ue::to_utf8(c->GetName());
            near_list += (near_list.empty() ? "" : " | ") + chain + " d=" + std::to_string(static_cast<int>(dist)) + (chr.empty() ? "" : " chr=" + chr);
        }
        if (ue::is_a(a, STR("BP_jRPG_NPC_Base_C")) || ue::is_a(a, STR("BP_jRPG_Enemy_World_Base_Seamless_C")) ||
            ue::is_a(a, STR("BP_jRPG_Character_World_C")))
            continue;
        const bool follower_cls = cls.find("Follow") != std::string::npos || cls.find("Companion") != std::string::npos;
        if (dist > 2500.0 || (chr.empty() && !follower_cls)) continue;
        follower_visible(a, false);
        g_followers_hidden.insert(a);
        ++hidden;
        names += (names.empty() ? "" : ", ") + (chr.empty() ? cls : chr + " (" + cls + ")");
    }
    if (scan_log) {
        g_followers_scan_level = level;
        log().write("roam_characters_near", JsonObj().add("level", level).add("n", static_cast<int>(all.size())).add("near", near_list));
    }
    if (hidden) {
        bump("roam_followers_hidden", hidden);
        log().write("roam_followers_hidden", JsonObj().add("n", hidden).add("chars", names).add("level", level));
    }
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
        // Fork (histoire partagée) : un seul niveau pour tous ; jamais de voyage vers un autre niveau.
        ui::toast("roam_other_level", {}, 4.0);
        log().write("roam_travel", JsonObj().add("to", static_cast<int>(player)).add("why", why).add("level", p.level).add("refused", "other_level"));
        return;
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
    roam_line_tick();
    const std::string level = game::current_level();
    // Local level changed: the puppets were destroyed with it. No explore pawn (battle, menus) gives an empty level and is
    // not a change: forgetting the actor there left the old puppet standing where the fight began and spawned a second
    // one after it (playtest 2026-11-16, "motionless Gustave").
    const bool map_change = game::map_change_in_progress();
    if (map_change || (!level.empty() && level != g_level)) {
        for (auto& [id, pp] : g_puppets) { pp.actor = nullptr; pp.hidden = false; }
        g_followers_hidden.clear();                       // gone with the level
        g_level = map_change ? std::string() : level;
    }
    const bool battle = game::in_battle();
    if (battle && !map_change) for (auto& [id, pp] : g_puppets) set_hidden(pp, true);   // not in the arena's shot
    const bool can_draw = game::explore_pawn() && !battle && !map_change;
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

    followers_tick(can_draw);

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

// --- Répliques de dialogue partagées (F9) -------------------------------------------------------------
// La réplique part avec sa voix (PlaySoundForCharacter suit SetDialogueLines) ; sans voix après 0,3 s, elle part seule.
namespace {
proto::DialogueLine g_line;
double g_line_at = -1;
uint32_t g_line_seq = 0;

void line_publish()
{
    if (g_line_at < 0) return;
    g_line_at = -1;
    s.net.publish(g_line, true);
    bump("roam_lines_published");
}

std::string short_text(const std::string& t) { return t.size() > 300 ? t.substr(0, 297) + "..." : t; }
}

void roam_line_local(const std::string& text, const std::string& speaker)
{
    line_publish();   // réplique précédente restée sans voix
    if (text.empty()) return;
    g_line = proto::DialogueLine{s.me, ++g_line_seq, speaker, short_text(text), {}};
    g_line_at = s.now;
}

void roam_line_sound(const std::string& sound_path)
{
    if (g_line_at < 0) return;
    g_line.sound = sound_path;
    line_publish();
}

void roam_line_tick()
{
    if (g_line_at >= 0 && s.now - g_line_at > 0.3) line_publish();
}

void roam_line_remote(const proto::DialogueLine& m)
{
    if (m.player == s.me || s.roam) return;   // fork : plus montrées (histoire partagée)
    // Pendant sa propre scène, rien par-dessus (sous-titres et voix mêlés).
    if (game::in_dialogue() || game::playing_cinematic() || game::in_battle()) { bump("roam_lines_skipped_busy"); return; }
    const std::string who = s.net.name_of ? s.net.name_of(m.player) : std::to_string(m.player);
    const double secs = std::clamp(2.0 + static_cast<double>(m.text.size()) * 0.06, 3.0, 12.0);
    ui::toast("roam_line", {who, m.speaker.empty() ? "?" : m.speaker, m.text}, secs);
    bool played = false;
    if (!m.sound.empty()) {
        UObject* snd = game::load_asset(ue::from_utf8(m.sound).c_str());
        UObject* gps = ue::find_object(STR("/Script/Engine.Default__GameplayStatics"));
        ue::Call c(gps, STR("PlaySound2D"));
        if (snd && c && game::controller()) {
            const float one = 1.0f;
            c.set_object(STR("WorldContextObject"), game::controller()).set_object(STR("Sound"), snd)
                .set_raw(STR("VolumeMultiplier"), &one).set_raw(STR("PitchMultiplier"), &one);
            played = c.invoke();
        }
    }
    bump("roam_lines_shown");
    if (played) bump("roam_lines_voiced");
    log().write("roam_line_shown", JsonObj().add("player", static_cast<int>(m.player)).add("speaker", m.speaker).add("text", m.text).add("voiced", played));
}

// Interactions d'un autre joueur rejouées ici (F9 : « ce qu'un suiveur fait dans le monde compte pour l'équipe ») :
// seulement les objets d'état du monde (coffres, objets ramassables). Pas les PNJ (dialogue chez tous), ennemis
// (combat), prises d'escalade, cordes, grappins (mouvement), zones de déclenchement (scènes : SceneStart), documents
// (fenêtre de lecture chez tous).
bool roam_shared_interaction(UObject* target)
{
    if (!target) return false;
    for (const wchar_t* no : {STR("BP_jRPG_NPC_Base_C"), STR("BP_jRPG_NPC_Dialog_C"), STR("BP_jRPG_Enemy_World_Base_Seamless_C"),
                              STR("BP_GameActionTriggerZone_C"), STR("BP_DocumentActor_C"), STR("BP_ReadingCube_C"), STR("BP_GrapplePoint_C"),
                              STR("BP_Holds_C"), STR("BP_GPE_Rope_C"), STR("BP_LightSafeArea_C")})
        if (ue::is_a(target, no)) return false;
    return ue::is_a(target, STR("BP_Chest_Regular_C")) || ue::is_a(target, STR("BP_InteractionObjectBase_C"));
}

bool roam_movement_interaction(UObject* target)
{
    if (!target) return false;
    for (const wchar_t* c : {STR("BP_jRPG_Enemy_World_Base_Seamless_C"), STR("BP_GrapplePoint_C"), STR("BP_Holds_C"), STR("BP_GPE_Rope_C"),
                             STR("BP_LightSafeArea_C")})
        if (ue::is_a(target, c)) return true;
    return false;
}

// --- Histoire partagée (docs/PLAN-SHARED-STORY.md) ------------------------------------------------
// Un événement d'histoire (conversation, cinématique, document) à la fois. Celui qui le lance le mène : l'hôte
// l'accorde (StoryEvent à tous), les autres sont rassemblés près de lui, ses interactions, choix et passages sont
// rejoués chez eux, leurs entrées sont bloquées (is_pilot faux). Le meneur le termine quand plus rien n'est en cours
// chez personne (barrière : NarrativeState de chacun).
namespace {

// Perdant d'un départ simultané : sa conversation locale est fermée (il rejoint celle du meneur).
bool close_local_dialogue()
{
    UObject* sys = game::dialogue_system();
    if (!sys || !game::in_dialogue()) return false;
    UObject* dlg = ue::get_object(sys, STR("ActiveDialogue"));
    ue::Call c(sys, STR("EndDialogue"));
    if (!c || !dlg) return false;
    const std::string param = c.set_first_object(dlg);   // nom du paramètre inconnu : premier paramètre objet
    log().write("story_close_dialogue", JsonObj().add("param", param));
    if (param.empty()) return false;
    s.replaying = true;
    const bool ok = c.invoke();
    s.replaying = false;
    return ok;
}

void story_set(uint8_t leader, uint32_t seq, const char* why)
{
    const uint8_t was = s.story_leader;
    if (seq) s.story_seq = std::max(s.story_seq, seq);
    s.story_leader = leader;
    s.story_quiet_since = -1;
    s.story_end_sent_at = -1;
    if (leader != 0xFF && leader != was) { s.story_at = s.now; bump("story_events"); }
    log().write("story_event", JsonObj().add("leader", static_cast<int>(leader)).add("was", static_cast<int>(was)).add("why", why).add("seq", s.story_seq));
    if (s.story_requested && leader != 0xFF) {
        s.story_requested = false;
        if (leader != s.me) {   // départ simultané perdu
            const bool closed = close_local_dialogue();
            bump("story_event_conflicts");
            log().write("story_event_conflict", JsonObj().add("winner", static_cast<int>(leader)).add("closed_local", closed));
        }
    }
    if (leader != 0xFF && leader != s.me && leader != was) roam_gather(leader);
    if (leader == 0xFF && was != 0xFF) bump("story_events_ended");
}

void story_publish(uint8_t leader, const char* why)
{
    story_set(leader, ++s.story_seq, why);
    s.net.publish(proto::StoryEvent{s.story_seq, leader}, true);
}
} // namespace

bool story_active() { return s.roam && s.story_leader != 0xFF; }

void story_begin_local(const char* why)
{
    if (!s.roam || !sharing()) return;
    if (s.host) { story_on_request(s.me); return; }
    if (s.story_requested) return;
    s.story_requested = true;
    s.story_request_at = s.now;
    s.net.publish(proto::PilotRequest{s.me}, true);
    bump("story_requests");
    log().write("story_request", JsonObj().add("why", why));
}

void story_on_request(uint8_t from)
{
    if (!s.host) return;
    if (s.story_leader != 0xFF && s.story_leader != from) {   // un autre mène déjà : le demandeur le rejoindra
        bump("story_requests_refused");
        log().write("story_request_refused", JsonObj().add("from", static_cast<int>(from)).add("leader", static_cast<int>(s.story_leader)));
        if (from == s.me) { story_set(s.story_leader, 0, "refused"); return; }
        s.net.send_to(from, proto::StoryEvent{s.story_seq, s.story_leader});
        return;
    }
    if (s.story_leader == from) { if (from != s.me) s.net.send_to(from, proto::StoryEvent{s.story_seq, from}); return; }
    if (from == s.me) s.story_requested = false;
    story_publish(from, "granted");
}

void story_on_event(uint8_t from, const proto::StoryEvent& e)
{
    if (s.host) {
        // Client meneur : fin de son événement (seul message StoryEvent accepté d'un client).
        if (e.leader == 0xFF && (from == s.story_leader || from == s.me) && s.story_leader != 0xFF) story_publish(0xFF, from == s.me ? "leader_left" : "leader_done");
        return;
    }
    if (e.seq && e.seq < s.story_seq && e.leader != 0xFF) return;   // ancien
    story_set(e.leader, e.seq, "host");
}

bool story_relay_ok(uint8_t from, const std::string& actor)
{
    if (actor.rfind("freeaim:", 0) == 0) return true;
    if (s.story_leader != 0xFF && from == s.story_leader) return true;
    UObject* target = ue::find_object(ue::from_utf8(actor).c_str());
    if (!target) return s.story_leader == 0xFF;   // pas chargé ici : hors événement, relayé (le receveur filtre)
    if (roam_shared_interaction(target) || roam_movement_interaction(target)) return true;
    // Interaction d'histoire d'un non-meneur : refusée pendant un autre événement (perdant d'un départ simultané).
    return s.story_leader == 0xFF;
}

void story_tick()
{
    if (!s.roam) return;
    if (s.story_requested && s.now - s.story_request_at > 5.0) {   // pas de réponse (hôte occupé, perdu)
        s.story_requested = false;
        bump("story_requests_lost");
        log().write("story_request_lost", JsonObj());
    }
    if (s.story_leader == 0xFF) return;
    if (s.host && s.now - s.story_at > 900.0) {   // filet : jamais bloqués plus de 15 min
        bump("story_events_timeout");
        story_publish(0xFF, "timeout");
        return;
    }
    if (s.story_leader != s.me) return;
    // Meneur : fin quand plus rien n'est en cours ici ni chez les autres (dialogue, cinématique, marchand, combat).
    bool busy = s.local_narr || game::in_dialogue() || game::playing_cinematic() || game::in_battle() || s.choice_pending ||
                s.merchant_open || sc.pending.has_value() || !s.interactions.empty() || !s.choices.empty() || game::map_change_in_progress();
    for (auto& [p, on] : s.remote_narr)
        if (on && p != s.me) busy = true;
    if (busy) { s.story_quiet_since = -1; return; }
    if (s.story_quiet_since < 0) { s.story_quiet_since = s.now; return; }
    if (s.now - s.story_quiet_since < 2.5 || s.now - s.story_at < 2.0) return;
    if (s.host) { story_publish(0xFF, "done"); return; }
    if (s.story_end_sent_at < 0 || s.now - s.story_end_sent_at > 3.0) {
        s.story_end_sent_at = s.now;
        s.net.publish(proto::StoryEvent{s.story_seq, 0xFF}, true);
        log().write("story_end_request", JsonObj().add("seq", s.story_seq));
    }
}

void roam_reset()
{
    if (game::current_level() == g_level && !game::map_change_in_progress()) followers_restore("reset");   // else gone with their level
    g_followers_hidden.clear();
    for (auto& [id, pp] : g_puppets) destroy_puppet(pp, "reset");
    g_puppets.clear();
    g_travel = Travel{};
}

} // namespace e33::explore::detail
