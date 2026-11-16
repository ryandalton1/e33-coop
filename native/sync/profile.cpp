#include "profile.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "../ui/ui.hpp"
#include "builds.hpp"
#include "coop.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace fs = std::filesystem;

namespace e33::profile {
namespace {

const wchar_t* const kOutSlot = L"e33lanes/profile/_out";
const wchar_t* const kInSlot = L"e33lanes/profile/_in";

struct State {
    bool enabled = true;
    bool loaded = false;
    std::vector<uint8_t> blob;                 // profile characters (game-serialized)
    std::vector<std::string> party;            // profile party order
    // Current lane battle
    uint32_t battle = 0;
    bool active = false;                       // profile applied to the world's characters
    std::vector<uint8_t> world_blob;           // the world's characters before the battle
    std::vector<std::string> world_party;
    bool restore_pending = false;
    int end_result = -1;
    // Export
    bool auto_tried = false;
    double explore_since = -1;
    bool key_down = false;
} g;

void count(const char* name, int n = 1) { coop::add_counter(name, n); }

fs::path dir() { return fs::path(game::saves_dir()) / L"e33lanes" / L"profile"; }

std::string join(const std::vector<std::string>& v)
{
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

bool load()
{
    std::ifstream fb(dir() / L"party.sav", std::ios::binary);
    std::ifstream fp(dir() / L"party.txt");
    if (!fb || !fp) return false;
    g.blob.assign(std::istreambuf_iterator<char>(fb), std::istreambuf_iterator<char>());
    g.party.clear();
    std::string line;
    while (std::getline(fp, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) g.party.push_back(line);
    }
    g.loaded = !g.blob.empty() && !g.party.empty();
    return g.loaded;
}

bool save(const std::vector<uint8_t>& blob, const std::vector<std::string>& party, std::string& err)
{
    std::error_code ec;
    fs::create_directories(dir(), ec);
    {
        std::ofstream f(dir() / L"party.sav.tmp", std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        if (!f) { err = "write party.sav"; return false; }
    }
    {
        std::ofstream f(dir() / L"party.txt.tmp", std::ios::trunc);
        for (auto& n : party) f << n << "\n";
        if (!f) { err = "write party.txt"; return false; }
    }
    fs::rename(dir() / L"party.sav.tmp", dir() / L"party.sav", ec);
    if (ec) { err = "rename party.sav: " + ec.message(); return false; }
    fs::rename(dir() / L"party.txt.tmp", dir() / L"party.txt", ec);
    if (ec) { err = "rename party.txt: " + ec.message(); return false; }
    g.blob = blob;
    g.party = party;
    g.loaded = true;
    return true;
}

// Every character the profile holds (applied all; the game ignores those absent from the world).
std::vector<std::string> profile_names()
{
    std::vector<std::string> names = game::character_names();
    for (auto& p : g.party) if (std::find(names.begin(), names.end(), p) == names.end()) names.push_back(p);
    return names;
}

void restore(const char* why)
{
    std::string err;
    // 1. This battle's progression (XP, levels) goes to the profile. Not after a defeat (nothing gained) or an abandon.
    if (g.end_result == game::EndVictory) {
        std::vector<uint8_t> after;
        if (game::make_characters_blob(kOutSlot, after, err)) {
            if (save(after, g.party, err)) count("profile_progress_saved");
            else log().write("profile_error", JsonObj().add("what", "save progress: " + err));
        } else {
            log().write("profile_error", JsonObj().add("what", "capture progress: " + err));
        }
    }
    // 2. The world's characters and party back.
    std::vector<std::string> applied;
    const bool ok = game::apply_characters_blob(kInSlot, g.world_blob, game::character_names(), applied, err);
    std::string perr;
    const bool party_ok = g.world_party.empty() || game::set_party_names(g.world_party, perr);
    count(ok && party_ok ? "profile_restores" : "profile_restore_failures");
    builds::rebaseline();
    log().write("profile_restored", JsonObj().add("why", why).add("ok", ok).add("party_ok", party_ok).add("applied", static_cast<int64_t>(applied.size()))
                                       .add("err", err + perr).add("result", g.end_result));
    g.active = false;
    g.restore_pending = false;
    g.world_blob.clear();
}

} // namespace

void init()
{
    g.enabled = cmdline::arg(L"e33profile") != L"0";
    if (!g.enabled) return;
    const bool ok = load();
    log().write("profile_init", JsonObj().add("loaded", ok).add("party", join(g.party)).add("size", static_cast<int64_t>(g.blob.size())));
}

bool have() { return g.loaded; }
bool applied() { return g.active; }

bool export_now(std::string& err)
{
    std::vector<uint8_t> blob;
    if (!game::make_characters_blob(kOutSlot, blob, err)) return false;
    const auto party = game::party_names();
    if (party.empty()) { err = "empty party"; return false; }
    if (!save(blob, party, err)) return false;
    count("profile_exports");
    log().write("profile_exported", JsonObj().add("party", join(party)).add("size", static_cast<int64_t>(blob.size())));
    return true;
}

void tick(double now, bool in_session, bool coop_world)
{
    if (!g.enabled) return;
    const bool exploring = game::explore_pawn() && !game::in_battle() && !game::map_change_in_progress();
    // Back from a lane battle: progression saved, world restored (before builds::tick looks at the characters).
    if (g.restore_pending && exploring) restore("back to exploration");
    if (g.active && !in_session && exploring) { g.end_result = -1; restore("session over"); }

    // Ctrl+F9: export from the loaded solo save.
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (down && !g.key_down) {
        std::string err;
        if (coop_world || in_session) ui::toast("profile_export_refused", {}, 5.0);
        else if (!exploring) ui::toast("profile_export_not_now", {}, 5.0);
        else if (export_now(err)) ui::toast("profile_exported", {join(g.party)}, 5.0);
        else { ui::toast("profile_export_failed", {err}, 6.0); log().write("profile_error", JsonObj().add("what", "export: " + err)); }
    }
    g.key_down = down;

    // First solo game with the mod: export automatically (10 s of calm exploration).
    if (!g.loaded && !g.auto_tried && !in_session && !coop_world && exploring) {
        if (g.explore_since < 0) g.explore_since = now;
        else if (now - g.explore_since > 10.0) {
            g.auto_tried = true;
            std::string err;
            if (export_now(err)) ui::toast("profile_exported", {join(g.party)}, 5.0);
            else log().write("profile_error", JsonObj().add("what", "auto export: " + err));
        }
    } else if (!exploring) {
        g.explore_since = -1;
    }
}

bool apply_for_battle(uint32_t battle_id)
{
    if (!g.enabled || !g.loaded) { if (g.enabled) count("profile_missing"); return false; }
    if (g.active && g.battle == battle_id) return true;
    if (g.active) return true;                   // a retried battle keeps the profile applied
    std::string err;
    if (!game::make_characters_blob(kOutSlot, g.world_blob, err)) {
        count("profile_apply_failures");
        log().write("profile_error", JsonObj().add("what", "backup world: " + err));
        return false;
    }
    g.world_party = game::party_names();
    std::vector<std::string> applied;
    if (!game::apply_characters_blob(kInSlot, g.blob, profile_names(), applied, err)) {
        count("profile_apply_failures");
        log().write("profile_error", JsonObj().add("what", "apply: " + err));
        g.world_blob.clear();
        return false;
    }
    std::string perr;
    const bool party_ok = game::set_party_names(g.party, perr);
    g.active = true;
    g.battle = battle_id;
    g.restore_pending = false;
    g.end_result = -1;
    count("profile_applied");
    log().write("profile_applied", JsonObj().add("battle", static_cast<int64_t>(battle_id)).add("party", join(g.party)).add("party_ok", party_ok)
                                      .add("applied", static_cast<int64_t>(applied.size())).add("err", perr));
    return true;
}

std::vector<std::string> battle_party(const std::vector<std::string>& fallback) { return g.active && !g.party.empty() ? g.party : fallback; }

void on_battle_end(int result)
{
    if (!g.active) return;
    g.end_result = result;
    // A defeat may be retried (same battle, profile kept); the restore waits for exploration either way.
    g.restore_pending = true;
}

} // namespace e33::profile
