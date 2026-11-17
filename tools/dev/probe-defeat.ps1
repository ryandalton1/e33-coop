# Fork probe (2026-11-16): game without the coop mod + Lua dev mod; loads TEST_B, hooks flags/enemy/dialogue functions,
# fights the nearest world enemy, forces victory, records the enemy before/after. Result: runs\explore\probe-*.txt and
# runs\explore\explore.jsonl (probe_* events). Then talk to NPCs if asked; close the game when done.
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Expedition 33")
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$explore = Join-Path $repo "runs\explore"
$snip = Join-Path $repo "lua\E33Explore\snippets"
$remote = Join-Path $repo "tools\dev\remote.ps1"
function Remote([string]$file, [int]$timeout = 30) { powershell -NoProfile -File $remote -File $file -TimeoutSec $timeout }
function Code([string]$code, [int]$timeout = 30) { $f = Join-Path $env:TEMP "e33-probe-cmd.lua"; [IO.File]::WriteAllText($f, $code); Remote $f $timeout }  # -Code loses its quotes
if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "Close the game first" }
New-Item -ItemType Directory -Force $explore | Out-Null
foreach ($f in "cmd.lua", "cmd.out", "cmd.tmp") { Remove-Item (Join-Path $explore $f) -ErrorAction SilentlyContinue }
Add-Content (Join-Path $explore "explore.jsonl") ('{"ev":"probe_start","data":"' + (Get-Date -Format s) + '"}')
& (Join-Path $repo "tools\reset-env\reset-env.ps1") -GameDir $GameDir -LuaMods @("E33Explore") | Out-Null
& "C:\Program Files (x86)\Steam\steam.exe" -applaunch 1903340 -windowed -ResX=1280 -ResY=720
Write-Output "game starting"
Start-Sleep 40
for ($i = 0; $i -lt 60; $i++) { if ((Code 'local m = FindFirstOf("WBP_MM_MainMenu_C"); return tostring(m ~= nil and m:IsValid())' 10) -match "true") { break }; Start-Sleep 3 }
Start-Sleep 4
Remote (Join-Path $snip "load_test_b.lua")
Start-Sleep 20
for ($i = 0; $i -lt 60; $i++) { if ((Code 'local p = FindFirstOf("BP_jRPG_Character_World_C"); return tostring(p ~= nil and p:IsValid())' 10) -match "true") { break }; Start-Sleep 3 }
Start-Sleep 12
Remote (Join-Path $snip "probe_defeat.lua") | Set-Content (Join-Path $explore "probe-hooks.txt")
Remote (Join-Path $snip "world_info.lua") | Set-Content (Join-Path $explore "probe-world-before.txt")
Remote (Join-Path $snip "world_battle.lua") | Set-Content (Join-Path $explore "probe-battle.txt")
Remote (Join-Path $snip "enemy_state.lua") | Set-Content (Join-Path $explore "probe-enemy-before.txt")
Start-Sleep 5
for ($i = 0; $i -lt 30; $i++) { if ((Code 'local b = FindFirstOf("AC_jRPG_BattleManager_C"); return tostring(b ~= nil and b:IsValid() and b.BattleInProgress)' 10) -match "true") { break }; Start-Sleep 2 }
Start-Sleep 15
Code 'local r = {}; for _, b in ipairs(FindAllOf("AC_jRPG_BattleManager_C") or {}) do if b:IsValid() then local ok, err = pcall(function() b:ForceBattleEnd(1) end); r[#r+1] = b:GetFullName() .. " " .. tostring(ok) .. " " .. tostring(err) end end; return table.concat(r, "\n")' | Set-Content (Join-Path $explore "probe-victory.txt")
Start-Sleep 25
for ($i = 0; $i -lt 10; $i++) { Code 'return "x"' 5 | Out-Null; Start-Sleep 2 }
Remote (Join-Path $snip "enemy_state.lua") | Set-Content (Join-Path $explore "probe-enemy-after.txt")
Remote (Join-Path $snip "world_info.lua") | Set-Content (Join-Path $explore "probe-world-after.txt")
Write-Output "probe done"
