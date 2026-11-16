-- LaneProbe v3: one-PC viability test for lane combat (brief, spike S1 + core of the lane merge).
--
-- Question: will the game accept (a) enemy HP scaled 3x and (b) enemy damage and kills that
-- come from outside the local fight?
--
-- Two "phantom teammates" copy your damage: each time an enemy loses HP, the probe takes 2x
-- that amount more within ~0.1 s. With 3x HP and 3x damage the fight should feel normal length.
--
-- History:
--   v1 polled with FindFirstOf 10x/s (lag) and merged at the next turn's start, so an enemy
--      killed by the merge had already been picked to act and attacked while invisible.
--   v2 used RegisterHook on OnTurnStart/OnTurnEnded/StartBattleEndFlow. StartBattleEndFlow runs
--      mid-fight (disarmed the probe), and the hooks coincided with ~12 hero turns in a row.
--   v3: no hooks. Enemies are found once on F7; the poll only reads properties. Phantom damage is
--      applied as hits land, so a phantom kill happens during your action, before the next pick.
--
--   F7  arm, during your first turn in a fight
--   F8  print enemy state now
--   F9  disarm
--
-- Every line goes to ue4ss/UE4SS.log with the prefix [LaneProbe].

local MULT     = 3.0   -- enemy HP multiplier (N players)
local PHANTOMS = 2     -- remote players simulated
local HP_STAT  = 1     -- E_jRPG_StatType: 1 = HP (GAME-INTERNALS.md section 9)

local function log(fmt, ...) print(string.format("[LaneProbe] " .. fmt .. "\n", ...)) end

local armed, observe = false, false
local the_bm = nil
local foes = {}         -- { spot, ch, st, addr, name, last, killed }
local last_actor = 0
local c = { hero_turns = 0, enemy_turns = 0, hits = 0, phantom_dmg = 0, phantom_kills = 0, dead_actor_turns = 0, sethp_fallbacks = 0, errors = 0 }

local function find_bm()
    local pc = FindFirstOf("BP_jRPG_Controller_World_C")
    if not pc or not pc:IsValid() then return nil end
    local m = pc.AC_jRPG_BattleManager
    return (m and m:IsValid()) and m or nil
end

local function collect(m)
    local out = {}
    m.Enemies:ForEach(function(_, e)
        local ch = e:get()
        if ch and ch:IsValid() then
            local st = ch.AC_jRPG_CharacterStats
            if st and st:IsValid() then
                table.insert(out, { spot = ch.BattleSpotIndex, ch = ch, st = st, addr = ch:GetAddress(),
                                    name = ch:GetClass():GetFName():ToString() })
            end
        end
    end)
    return out
end

local function hp_stat(map, mult)
    local found
    map:ForEach(function(k, v)
        if k:get() == HP_STAT then
            if mult then v:set(v:get() * mult) end
            found = v:get()
        end
    end)
    return found
end

local function max_hp(st)
    local ok, v = pcall(function() local o = {}; st:GetMaxHP(o); return o.MaxHP or o.ReturnValue end)
    if ok and type(v) == "number" then return v end
    return hp_stat(st.CharacterCurrentStats)
end

local function set_hp(st, hp)
    local ok = pcall(function() st:SetHP(hp) end)
    if not ok then c.sethp_fallbacks = c.sethp_fallbacks + 1; st.CurrentHP = hp end
end

local function is_dead(st)
    local ok, d = pcall(function() return st:IsDead() end)
    return (ok and d) or st.CurrentHP <= 0
end

local function dump(tag, list)
    for _, f in ipairs(list) do
        if f.st:IsValid() then
            log("%s spot=%d %s hp=%.0f max=%.0f dead=%s", tag, f.spot, f.name, f.st.CurrentHP, max_hp(f.st) or -1, tostring(is_dead(f.st)))
        end
    end
end

local function summary(why)
    log("SUMMARY (%s, %s) hero_turns=%d enemy_turns=%d hits=%d phantom_dmg=%.0f phantom_kills=%d dead_actor_turns=%d sethp_fallbacks=%d errors=%d",
        why, observe and "OBSERVE" or "PROBE", c.hero_turns, c.enemy_turns, c.hits, c.phantom_dmg, c.phantom_kills, c.dead_actor_turns, c.sethp_fallbacks, c.errors)
end

local function tick()
    if not the_bm:IsValid() then armed = false; summary("battle over"); return end

    local alive = 0
    for _, f in ipairs(foes) do
        if not f.killed and f.st:IsValid() then
            local hp = f.st.CurrentHP
            if observe then
                if hp <= 0 then f.killed = true end
                f.last = hp
            elseif hp < f.last and hp > 0 then
                local d = f.last - hp
                local hp1 = math.max(0, hp - d * PHANTOMS)
                set_hp(f.st, hp1)
                c.hits = c.hits + 1
                c.phantom_dmg = c.phantom_dmg + (hp - hp1)
                if hp1 <= 0 then
                    local ok = pcall(function() f.st:SetDead() end)
                    f.killed = true
                    c.phantom_kills = c.phantom_kills + 1
                    log("HIT spot=%d local=%.0f phantom=%.0f hp %.0f -> 0  KILL (SetDead %s)", f.spot, d, d * PHANTOMS, hp, ok and "ok" or "FAILED")
                else
                    log("HIT spot=%d local=%.0f phantom=%.0f hp %.0f -> %.0f", f.spot, d, d * PHANTOMS, hp, f.st.CurrentHP)
                end
                f.last = f.st.CurrentHP
            elseif hp <= 0 then
                f.killed = true
                log("LOCAL KILL spot=%d (your hit finished it)", f.spot)
            else
                f.last = hp
            end
        end
        if not f.killed then alive = alive + 1 end
    end

    local cc = the_bm.CurrentCharacter
    local addr = (cc and cc:IsValid()) and cc:GetAddress() or 0
    if addr ~= last_actor then
        last_actor = addr
        if addr ~= 0 then
            local enemy = false
            for _, f in ipairs(foes) do if f.addr == addr then enemy = true end end
            log("TURN %s %s", enemy and "ENEMY" or "HERO ", cc:GetFName():ToString())
            if enemy then c.enemy_turns = c.enemy_turns + 1 else c.hero_turns = c.hero_turns + 1 end
        end
        for _, f in ipairs(foes) do
            if f.addr == addr and f.killed then
                c.dead_actor_turns = c.dead_actor_turns + 1
                log("DEAD_ACTOR_TURN spot=%d %s (a dead enemy is acting)", f.spot, f.name)
            end
        end
    end

    if alive == 0 then armed = false; summary("all enemies dead") end
end

local function arm(obs)
    ExecuteInGameThread(function()
        if armed then log("already armed"); return end
        the_bm = find_bm()
        foes = the_bm and collect(the_bm) or {}
        if #foes == 0 then log("no fight in progress"); return end
        for k in pairs(c) do c[k] = 0 end
        observe = obs
        dump("BEFORE", foes)
        for _, f in ipairs(foes) do
          f.last, f.killed = f.st.CurrentHP, false
          if not observe then
            local hp0, max0 = f.st.CurrentHP, max_hp(f.st)
            pcall(hp_stat, f.st.CharacterBaseStats, MULT)
            hp_stat(f.st.CharacterCurrentStats, MULT)
            local max1 = max_hp(f.st)
            set_hp(f.st, hp0 * MULT)
            f.last, f.killed = f.st.CurrentHP, false
            log("SCALE spot=%d hp %.0f -> %.0f  max %.0f -> %.0f  %s", f.spot, hp0, f.st.CurrentHP, max0 or -1, max1 or -1,
                (math.abs(f.st.CurrentHP - hp0 * MULT) < 1 and math.abs((max1 or 0) - (max0 or 0) * MULT) < 1) and "OK" or "MISMATCH")
          end
        end
        last_actor = -1   -- log the current turn on the first tick
        armed = true
        if observe then log("OBSERVE v5: logging turns only, enemies untouched")
        else log("ARMED v5 x%.1f HP, %d phantom teammates", MULT, PHANTOMS) end
    end)
end

-- v5: the poll loop starts once at mod load, like upstream's E33Explore. v4 started it from inside
-- an ExecuteInGameThread callback on F7, and one run crashed (C++ exception in msvcp140) right then.
LoopAsync(100, function()
    if armed then
        ExecuteInGameThread(function()
            if not armed then return end
            local ok, err = pcall(tick)
            if not ok then c.errors = c.errors + 1; log("ERROR %s", tostring(err)) end
        end)
    end
    return false
end)

RegisterKeyBind(Key.F6, function() arm(true) end)
RegisterKeyBind(Key.F7, function() arm(false) end)
RegisterKeyBind(Key.F8, function() ExecuteInGameThread(function() dump("STATE", foes) end) end)
RegisterKeyBind(Key.F9, function() ExecuteInGameThread(function() if armed then armed = false; summary("disarmed") end end) end)

log("loaded v5: F6 observe only, F7 arm probe, F8 state, F9 disarm")
