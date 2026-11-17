-- Fork E33Lanes probe (2026-11-16): how a beaten world enemy stays gone, and how story dialogues can be told apart.
-- Hooks: persistent flags written, world-enemy battle end functions, dialogue start (asset path), save writes.
-- Also lists the functions of the world-enemy class (names containing defeat/kill/dead/battle/despawn/respawn/save).
local res = {}
_G.E33ProbeHooks = _G.E33ProbeHooks or {}
local function hook(path, fn)
  if _G.E33ProbeHooks[path] then return end
  local ok, err = pcall(function() RegisterHook(path, fn) end)
  if ok then _G.E33ProbeHooks[path] = true end
  res[#res+1] = (ok and "ok   " or "FAIL ") .. path:match("([^%.]+:.*)$") .. (ok and "" or (" " .. tostring(err)))
end
local function name_of(o)
  local s = "?"
  pcall(function() s = o:GetFullName() end)
  return s
end

hook("/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_GI_Custom.BP_jRPG_GI_Custom_C:WritePersistentFlag", function(ctx, id, value)
  local n, v = "?", "?"
  pcall(function() n = id:get():GetFullName() end)
  pcall(function() v = tostring(value:get()) end)
  E33Log("probe_flag", n .. " = " .. v)
end)

local enemy = "/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C:"
local cls = StaticFindObject("/Game/jRPGTemplate/Blueprints/Basics/BP_jRPG_Enemy_World_Base_Seamless.BP_jRPG_Enemy_World_Base_Seamless_C")
local fnames = {}
if cls and cls:IsValid() then
  cls:ForEachFunction(function(f)
    local n = f:GetFName():ToString()
    fnames[#fnames+1] = n
    local l = n:lower()
    if l:find("defeat") or l:find("kill") or l:find("dead") or l:find("battle") or l:find("despawn") or l:find("respawn")
       or l:find("save") or l:find("hide") or l:find("destroy") or l:find("finish") or l:find("vanish") or l:find("death") then
      hook(enemy .. n, function(ctx)
        E33Log("probe_enemy_fn", n .. " on " .. name_of(ctx:get()))
      end)
    end
  end)
end
res[#res+1] = "enemy class functions (" .. #fnames .. "): " .. table.concat(fnames, ", ")

hook("/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:ActivateDialogue", function(ctx, d)
  local n, owner = "?", "?"
  pcall(function() n = d:get():GetFullName() end)
  pcall(function() owner = ctx:get():GetOwner():GetFullName() end)
  E33Log("probe_dialogue", n .. " | owner " .. owner)
  -- every simple field of the dialogue asset and of the owner's dialogue component (looking for a story marker)
  for _, pair in ipairs({ { "asset", function() return d:get() end }, { "comp", function() return ctx:get() end } }) do
    pcall(function()
      local o = pair[2]()
      local c = o:GetClass()
      local out = {}
      while c and c:IsValid() do
        local cname = c:GetFName():ToString()
        if cname == "Object" or cname == "ActorComponent" or cname == "DataAsset" or cname == "PrimaryDataAsset" then break end
        c:ForEachProperty(function(p)
          local pn = p:GetFName():ToString()
          local v = "?"
          pcall(function()
            local x = o[pn]
            local t = type(x)
            if t == "boolean" or t == "number" or t == "string" then v = tostring(x)
            elseif t == "userdata" then
              local s = nil
              pcall(function() s = x:ToString() end)
              if not s then pcall(function() s = x:GetFullName() end) end
              v = s or "<obj>"
            end
          end)
          out[#out+1] = pn .. "=" .. v:sub(1, 120)
        end)
        c = c:GetSuperStruct()
      end
      E33Log("probe_dialogue_" .. pair[1], table.concat(out, " ; "))
    end)
  end
end)
hook("/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C:EndDialogue", function(ctx)
  E33Log("probe_dialogue_end", name_of(ctx:get()))
end)
hook("/Game/Gameplay/Save/BP_SaveManager.BP_SaveManager_C:SaveGame", function(ctx) E33Log("probe_save", "SaveGame") end)
return table.concat(res, "\n")
