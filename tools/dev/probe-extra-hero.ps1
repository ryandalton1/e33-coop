# Fork probe (2026-11-17): can a teammate's hero be shown on our battlefield doing their moves? Game without the coop mod
# + Lua dev mod: loads TEST_B, starts a test fight (Lune + Gustave vs Lancelier Alpha), waits for the first hero turn,
# runs lua\E33Explore\snippets\probe_extra_hero.lua. Result: runs\explore\probe-extra-hero.txt and runs\explore\explore.jsonl
# (probe_hero_* events). Watch the screen: a second Lune should appear behind yours and play a skill animation for 6 s.
# Afterwards reinstall E33Lanes (install.ps1 or the player zip): reset-env leaves only UE4SS + the Lua dev mod.
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Expedition 33")
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$explore = Join-Path $repo "runs\explore"
$snip = Join-Path $repo "lua\E33Explore\snippets"
$remote = Join-Path $repo "tools\dev\remote.ps1"
function Remote([string]$file, [int]$timeout = 30) { powershell -NoProfile -File $remote -File $file -TimeoutSec $timeout }
function Code([string]$code, [int]$timeout = 30) { $f = Join-Path $env:TEMP "e33-probe-cmd.lua"; [IO.File]::WriteAllText($f, $code); Remote $f $timeout }
if (Get-Process "SandFall-Win64-Shipping" -ErrorAction SilentlyContinue) { throw "Close the game first" }
New-Item -ItemType Directory -Force $explore | Out-Null
foreach ($f in "cmd.lua", "cmd.out", "cmd.tmp") { Remove-Item (Join-Path $explore $f) -ErrorAction SilentlyContinue }
Add-Content (Join-Path $explore "explore.jsonl") ('{"ev":"probe_start","data":"extra_hero ' + (Get-Date -Format s) + '"}')
& (Join-Path $repo "tools\reset-env\reset-env.ps1") -GameDir $GameDir -LuaMods @("E33Explore") | Out-Null
& "C:\Program Files (x86)\Steam\steam.exe" -applaunch 1903340 -windowed -ResX=1280 -ResY=720
Write-Output "game starting"
Start-Sleep 40
& (Join-Path $PSScriptRoot "explore-battle.ps1") -Party "Lune,Frey" -Encounter "SM_Lancelier_Alpha"
# First hero turn: the battle is set up and the heroes stand in their spots.
for ($i = 0; $i -lt 60; $i++) {
    if ((Code 'local b = FindFirstOf("AC_jRPG_BattleManager_C"); local c; pcall(function() c = b.CurrentCharacter end); local p = false; pcall(function() p = c.IsPlayerControlledCharacter end); return tostring(p)' 10) -match "true") { break }
    Start-Sleep 2
}
Start-Sleep 3
Remote (Join-Path $snip "probe_extra_hero.lua") 60 | Set-Content (Join-Path $explore "probe-extra-hero.txt")
Start-Sleep 10
Get-Content (Join-Path $explore "probe-extra-hero.txt")
Write-Output "probe done: close the game, then reinstall E33Lanes"
