-- Fork probe (2026-11-16): which function shows each dialogue line, and with what text/speaker. Hooks every function of
-- the dialogue overlay widget and the dialogue component whose name suggests a line, and logs each parameter's text.
local res = {}
_G.E33LineHooks = _G.E33LineHooks or {}
local function txt(v)
  local s = nil
  pcall(function() local x = v:get(); local t = type(x)
    if t == "string" or t == "number" or t == "boolean" then s = tostring(x)
    elseif t == "userdata" then
      pcall(function() s = x:ToString() end)
      if not s then pcall(function() s = x:GetFullName() end) end
    end
  end)
  return (s or "?"):sub(1, 200)
end
local classes = {
  "/Game/Gameplay/DialogueSystem/BP_DialogueSystemComponent.BP_DialogueSystemComponent_C",
}
for _, w in ipairs(FindAllOf("WBP_DialogueOverlay_C") or {}) do
  pcall(function() classes[#classes + 1] = w:GetClass():GetFullName():match("^%S+%s+(.*)$") end)
  break
end
local seen = {}
for _, cpath in ipairs(classes) do
  local cls = StaticFindObject(cpath)
  if cls and cls:IsValid() and not seen[cpath] then
    seen[cpath] = true
    local names = {}
    cls:ForEachFunction(function(f)
      local n = f:GetFName():ToString()
      local l = n:lower()
      names[#names + 1] = n
      if (l:find("line") or l:find("text") or l:find("speaker") or l:find("subtitle") or l:find("display") or l:find("show")
          or l:find("next") or l:find("play") or l:find("voice") or l:find("audio")) and not l:find("^bndevt") then
        local path = cpath .. ":" .. n
        if not _G.E33LineHooks[path] then
          local ok = pcall(function()
            RegisterHook(path, function(ctx, ...)
              local a = {}
              for i, p in ipairs({ ... }) do a[#a + 1] = txt(p) end
              E33Log("probe_line_fn", n .. " (" .. table.concat(a, " | ") .. ")")
            end)
          end)
          if ok then _G.E33LineHooks[path] = true end
          res[#res + 1] = (ok and "ok   " or "FAIL ") .. path
        end
      end
    end)
    res[#res + 1] = cpath .. " functions: " .. table.concat(names, ", ")
  end
end
if #classes < 2 then res[#res + 1] = "WBP_DialogueOverlay_C not loaded yet (run again after the first dialogue)" end
return table.concat(res, "\n")
