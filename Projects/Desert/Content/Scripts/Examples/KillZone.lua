-- A kill volume (UE: a pain-causing volume / KillZ): the player's pawn that begins to overlap it dies through the
-- level's game rules (gameMode.kill, GameMode::Kill), and RestartPlayer brings a new pawn back at the PlayerStart
-- after the level's Respawn Delay. The volume's RigidBody states the Trigger collision profile, so OnBeginOverlap
-- arrives for the Pawn profile's capsule. Any other body passing through is left alone.

function OnBeginOverlap(other)
    -- The pawn is the body with the Character Controller; GameMode::Kill refuses any other by name.
    if gameMode.pawn() == nil or not other:has("CharacterController") then
        return
    end
    local ok, reason = gameMode.kill(other)
    if not ok then
        Log.warn(self:name() .. ": gameMode.kill refused: " .. tostring(reason))
    end
end
