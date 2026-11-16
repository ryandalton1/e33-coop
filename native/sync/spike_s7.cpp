// Spike S7: see spike_s7.hpp.
#include "spike_s7.hpp"

#include <cstdint>
#include <string>

#include "../common/cmdline.hpp"
#include "../common/jsonlog.hpp"
#include "../hooks/game.hpp"
#include "../hooks/hookbus.hpp"
#include "../hooks/log.hpp"
#include "../hooks/ue.hpp"
#include "coop.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

using namespace RC::Unreal;

namespace e33::spike_s7 {
namespace {

enum class Mode { Off, Observe, PauseActionFinished, PauseEndCharacterTurn, PauseTurnEnded };

struct State {
    Mode mode = Mode::Off;
    double hold_s = 5.0;
    int max_holds = 3;                 // per game session: enough for a verdict, keeps the fight playable
    uint64_t frame = 0;
    uint64_t t0 = 0;                   // first hooked event (ms)
    // Current hold
    bool holding = false;
    uint64_t hold_start_ms = 0;
    uint64_t hold_start_frame = 0;
    int turns_during = 0;              // StartCharacterTurn / OnTurnStart seen while paused
    int holds = 0;
    // After a release: did the next turn start?
    bool awaiting_next = false;
    uint64_t release_ms = 0;
} g;

uint64_t now_ms() { return GetTickCount64(); }

void count(const char* name, int n = 1) { coop::add_counter(name, n); }

const char* mode_name(Mode m)
{
    switch (m) {
    case Mode::Observe: return "observe";
    case Mode::PauseActionFinished: return "pause_af";
    case Mode::PauseEndCharacterTurn: return "pause_ect";
    case Mode::PauseTurnEnded: return "pause_te";
    default: return "off";
    }
}

std::string who(UObject* ch)
{
    if (!ch) return "";
    return game::char_class(ch);
}

UObject* current_character()
{
    UObject* bm = game::battle_manager();
    return bm ? ue::get_object(bm, STR("CurrentCharacter")) : nullptr;
}

void event(const char* fn, const char* phase, UObject* ch)
{
    const uint64_t t = now_ms();
    if (!g.t0) g.t0 = t;
    log().write("s7_event", JsonObj().add("fn", fn).add("phase", phase).add("who", who(ch)).add("hero", ch && game::is_player(ch))
                                .add("frame", static_cast<int64_t>(g.frame)).add("t_ms", static_cast<int64_t>(t - g.t0))
                                .add("holding", g.holding).add("paused", game::is_game_paused()));
}

void start_hold(const char* site, UObject* ch)
{
    if (g.holding || g.holds >= g.max_holds || !ch || !game::is_player(ch)) return;
    const bool ok = game::set_game_paused(true);
    g.holding = true;
    g.hold_start_ms = now_ms();
    g.hold_start_frame = g.frame;
    g.turns_during = 0;
    ++g.holds;
    count("s7_holds");
    if (!ok) count("s7_pause_call_failures");
    log().write("s7_hold_start", JsonObj().add("site", site).add("who", who(ch)).add("pause_ok", ok).add("paused", game::is_game_paused()));
}

void end_hold()
{
    const bool ok = game::set_game_paused(false);
    const uint64_t ms = now_ms() - g.hold_start_ms;
    const uint64_t frames = g.frame - g.hold_start_frame;
    g.holding = false;
    g.awaiting_next = true;
    g.release_ms = now_ms();
    count("s7_frames_during_hold", static_cast<int>(frames));
    count("s7_turns_during_hold", g.turns_during);
    if (g.turns_during == 0) count("s7_holds_clean");
    log().write("s7_hold_end", JsonObj().add("ms", static_cast<int64_t>(ms)).add("frames", static_cast<int64_t>(frames))
                                   .add("turns_during", g.turns_during).add("unpause_ok", ok).add("paused", game::is_game_paused()));
}

// A turn start: during a hold it means the pause did not stop the turn flow (hold failed).
void on_turn_start_event(const char* fn, UObject* ch)
{
    event(fn, "pre", ch);
    if (g.holding) { ++g.turns_during; log().write("s7_turn_during_hold", JsonObj().add("fn", fn).add("who", who(ch))); }
    if (g.awaiting_next && !g.holding) {
        g.awaiting_next = false;
        count("s7_turns_after_release");
        log().write("s7_next_turn_after_release", JsonObj().add("fn", fn).add("who", who(ch)).add("after_ms", static_cast<int64_t>(now_ms() - g.release_ms)));
    }
}

void subscribe()
{
    using game::path::fn;
    namespace p = game::path;
    auto& bus = HookBus::get();
    auto param_char = [](HookCtx& ctx, UFunction* f) -> UObject* {
        auto** slot = static_cast<UObject**>(ue::param_address(ctx, f, STR("Character")));
        return slot ? *slot : nullptr;
    };

    // Turn starts (the hold must keep these from running until it is released).
    bus.pre(fn(p::BattleMgr, L"StartCharacterTurn"), [param_char](HookCtx& ctx, UFunction* f) { on_turn_start_event("StartCharacterTurn", param_char(ctx, f)); });
    bus.post(fn(p::BattleChar, L"OnTurnStart"), [](HookCtx& ctx, UFunction*) { on_turn_start_event("OnTurnStart", ctx.Context); });

    // Turn-end candidates, BattleManager (GAME-INTERNALS.md 3.2). Names come from the dump; missing ones stay pending.
    bus.pre(fn(p::BattleMgr, L"End Character Turn"), [](HookCtx&, UFunction*) {
        UObject* ch = current_character();
        event("End Character Turn", "pre", ch);
        if (g.mode == Mode::PauseEndCharacterTurn) start_hold("End Character Turn", ch);
    });
    bus.pre(fn(p::BattleMgr, L"OnCharacterActionFinished"), [param_char](HookCtx& ctx, UFunction* f) {
        UObject* ch = param_char(ctx, f);
        if (!ch) ch = current_character();
        event("OnCharacterActionFinished", "pre", ch);
        if (g.mode == Mode::PauseActionFinished) start_hold("OnCharacterActionFinished", ch);
    });
    for (const wchar_t* name : {L"ProcessNextCharacterTurnEnd", L"OnTurnEndEventBegin", L"OnTurnEndEventEnd", L"ProcessAdditionalTurnEndEvent",
                                L"CheckBattleEnd", L"UpdateBattleTurns", L"UpdateCharacterNextTurns", L"StartCharacterTurn_Phase2"}) {
        const std::string n = ue::to_utf8(name);
        bus.pre(fn(p::BattleMgr, name), [n](HookCtx&, UFunction*) { event(n.c_str(), "pre", current_character()); });
    }

    // Turn-end candidates, character.
    bus.post(fn(p::BattleChar, L"OnTurnEnded"), [](HookCtx& ctx, UFunction*) {
        event("OnTurnEnded", "post", ctx.Context);
        if (g.mode == Mode::PauseTurnEnded) start_hold("OnTurnEnded", ctx.Context);
    });
    for (const wchar_t* name : {L"RequestAdditionalTurnEnd", L"FlushAdditionalTurnEndEvent"}) {
        const std::string n = ue::to_utf8(name);
        bus.pre(fn(p::BattleChar, name), [n](HookCtx& ctx, UFunction*) { event(n.c_str(), "pre", ctx.Context); });
    }
}

} // namespace

void init()
{
    const std::wstring m = cmdline::arg(L"e33s7");
    if (m == L"observe") g.mode = Mode::Observe;
    else if (m == L"pause_af") g.mode = Mode::PauseActionFinished;
    else if (m == L"pause_ect") g.mode = Mode::PauseEndCharacterTurn;
    else if (m == L"pause_te") g.mode = Mode::PauseTurnEnded;
    else return;
    if (const std::wstring h = cmdline::arg(L"e33s7hold"); !h.empty()) g.hold_s = std::wcstod(h.c_str(), nullptr);
    for (const char* c : {"s7_holds", "s7_holds_clean", "s7_turns_during_hold", "s7_frames_during_hold", "s7_turns_after_release", "s7_pause_call_failures"})
        coop::add_counter(c, 0);
    subscribe();
    log().write("s7_init", JsonObj().add("mode", mode_name(g.mode)).add("hold_s", g.hold_s));
}

void tick(double)
{
    if (g.mode == Mode::Off) return;
    ++g.frame;
    if (g.holding && now_ms() - g.hold_start_ms >= static_cast<uint64_t>(g.hold_s * 1000.0)) end_hold();
}

bool active() { return g.mode != Mode::Off; }

} // namespace e33::spike_s7
