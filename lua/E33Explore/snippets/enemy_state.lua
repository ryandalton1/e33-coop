-- Fork probe: state of the world enemy last fought (_G.E33Enemy), all simple properties, plus nearby enemy count.
local e = _G.E33Enemy
local r = {}
if not e then return "no E33Enemy" end
local ok = pcall(function() return e:IsValid() end)
r[#r+1] = "valid=" .. tostring(ok and e:IsValid())
if ok and e:IsValid() then
  pcall(function() r[#r+1] = "name=" .. e:GetFullName() end)
  pcall(function() r[#r+1] = "hidden=" .. tostring(e.bHidden) .. " collision=" .. tostring(e:GetActorEnableCollision()) end)
  local c = e:GetClass()
  while c and c:IsValid() do
    local cname = c:GetFName():ToString()
    if cname == "Actor" or cname == "Pawn" or cname == "Character" then break end
    pcall(function()
      c:ForEachProperty(function(p)
        local n = p:GetFName():ToString()
        local v = "?"
        pcall(function()
          local x = e[n]
          local t = type(x)
          if t == "boolean" or t == "number" or t == "string" then v = tostring(x)
          elseif t == "userdata" then
            local s = nil
            pcall(function() s = x:ToString() end)
            if not s then pcall(function() s = x:GetFullName() end) end
            v = s or "<obj>"
          end
        end)
        r[#r+1] = cname .. "." .. n .. "=" .. v
      end)
    end)
    c = c:GetSuperStruct()
  end
end
return table.concat(r, "\n")
