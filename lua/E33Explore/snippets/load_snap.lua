-- Phase 4 : recharge l'instantané e33lanes/SNAP (autosave coupée).
local gi = FindFirstOf("BP_jRPG_GI_Custom_C")
local sm = gi.SaveManager
sm.EnableAutoSave = false
local gps = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local sg = gps:LoadGameFromSlot("e33lanes/SNAP", 0)
assert(sg and sg:IsValid(), "SNAP introuvable")
sm:LoadGameFromSaveFile(sg, "e33lanes/SNAP")
return "loading " .. sg:GetFullName()
