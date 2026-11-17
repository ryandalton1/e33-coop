-- Fork E33Lanes probe (2026-11-17): can a teammate's hero be shown on our battlefield doing their moves? (level 3 of the
-- "watch your teammate's turn" idea). Run in a battle (tools/dev/probe-extra-hero.ps1). Read-only first, then one try:
--  1. BattleManager: functions/properties about spawning, reserve team, swapping, adding characters;
--  2. our heroes: class, functions about montages/skills/animations, the skeletal mesh and its anim instance;
--  3. spawn a second copy of our first hero's battle class 400 units behind it; is it counted by the battle
--     (BattleManager player list size before/after), is it visible, does it tick;
--  4. on that copy, play the first AnimMontage found whose name has the hero's name and "Skill" (or "Attack");
--  5. leave it 6 s, then destroy it. Everything is logged to runs\explore\explore.jsonl (probe_hero_*) and returned.
local out = {}
local function say(k, v) out[#out+1] = k .. ": " .. tostring(v); pcall(function() E33Log("probe_hero_" .. k, tostring(v)) end) end
local function name(o) local s = "nil"; pcall(function() s = o:GetFullName() end); return s end
local function short(o) local s = "nil"; pcall(function() s = o:GetFName():ToString() end); return s end

local bm = FindFirstOf("AC_jRPG_BattleManager_C")
if not bm or not bm:IsValid() then return "no battle manager (start a battle first)" end
local inprog = false; pcall(function() inprog = bm.BattleInProgress end)
say("battle_in_progress", inprog)

-- 1. BattleManager surface
local function list_funcs(cls, pats)
  local hits = {}
  pcall(function()
    cls:ForEachFunction(function(f)
      local n = f:GetFName():ToString()
      for _, p in ipairs(pats) do if n:lower():find(p) then hits[#hits+1] = n; break end end
    end)
  end)
  table.sort(hits)
  return table.concat(hits, ", ")
end
local function list_props(cls, pats)
  local hits = {}
  pcall(function()
    cls:ForEachProperty(function(p)
      local n = p:GetFName():ToString()
      for _, pt in ipairs(pats) do if n:lower():find(pt) then hits[#hits+1] = n; break end end
    end)
  end)
  table.sort(hits)
  return table.concat(hits, ", ")
end
local bmcls = bm:GetClass()
say("bm_funcs", list_funcs(bmcls, {"spawn", "reserve", "swap", "add", "replace", "join", "summon", "character", "team"}))
say("bm_props", list_props(bmcls, {"reserve", "player", "team", "character", "spot", "position"}))

-- 2. Our heroes
local function players()
  local t = {}
  pcall(function() bm.PlayerCharacters:ForEach(function(_, e) local o = e:get(); if o and o:IsValid() then t[#t+1] = o end end) end)
  return t
end
local heroes = players()
say("heroes", #heroes)
if #heroes == 0 then return table.concat(out, "\n") end
local hero = heroes[1]
local hcls = hero:GetClass()
say("hero", name(hero))
say("hero_funcs", list_funcs(hcls, {"montage", "skill", "anim", "play", "cinematic", "sequence", "spawn"}))
local mesh; pcall(function() mesh = hero.Mesh end)
local anim; pcall(function() anim = mesh:GetAnimInstance() end)
say("hero_mesh", name(mesh)); say("hero_anim", name(anim))
local before = #players()
say("bm_players_before", before)

-- 3. Spawn a copy behind the hero
local loc = hero:K2_GetActorLocation()
local rot = hero:K2_GetActorRotation()
local fwd = hero:GetActorForwardVector()
local at = {X = loc.X - fwd.X * 400, Y = loc.Y - fwd.Y * 400, Z = loc.Z}
local world = hero:GetWorld()
local copy
local ok, err = pcall(function() copy = world:SpawnActor(hcls, at, rot) end)
say("spawn_ok", ok and copy ~= nil and copy:IsValid()); if not ok then say("spawn_err", err) end
if copy and copy:IsValid() then
  say("copy", name(copy))
  local hidden = "?"; pcall(function() hidden = tostring(copy.bHidden) end); say("copy_hidden", hidden)
  local after = #players(); say("bm_players_after", after)
  local cmesh; pcall(function() cmesh = copy.Mesh end)
  local canim; pcall(function() canim = cmesh:GetAnimInstance() end)
  say("copy_anim", name(canim))
  -- 4. A skill montage of this hero
  local hname = short(hcls):gsub("^BP_", ""):gsub("_Battle.*$", "")
  local mont
  for _, m in ipairs(FindAllOf("AnimMontage") or {}) do
    local n = short(m)
    if n:find(hname) and (n:find("Skill") or n:find("Attack")) then mont = m; break end
  end
  say("montage", name(mont))
  if mont and canim then
    local r = 0
    local okm, errm = pcall(function() r = canim:Montage_Play(mont, 1.0, 0, 0.0, true) end)
    say("montage_play", tostring(okm) .. " len=" .. tostring(r) .. (okm and "" or (" " .. tostring(errm))))
  end
  -- 5. Clean up after 6 s
  ExecuteWithDelay(6000, function()
    ExecuteInGameThread(function()
      local still = #players()
      pcall(function() E33Log("probe_hero_players_6s", tostring(still)) end)
      pcall(function() copy:K2_DestroyActor() end)
      pcall(function() E33Log("probe_hero_destroyed", "yes") end)
    end)
  end)
end
return table.concat(out, "\n")
