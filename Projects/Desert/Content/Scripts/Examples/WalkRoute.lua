-- Scripted walk for a third-person pawn (FIELD-WINTER): instead of IA_Move, the pawn walks straight ahead for
-- Distance cm from where Play put it, then stops. The engine still owns the movement (Character Controller) and
-- the look (Humanoid locomotion graph fed Speed by the Character Controller); this only supplies the intent.

Properties = {
    Distance = Property.Float( 1500.0 ), -- cm walked before stopping
    StartDelay = Property.Float( 0.5 ), -- s standing still first, so the pawn settles on the ground
}

local start = nil
local elapsed = 0.0
local done = false

function OnUpdate(dt)
    elapsed = elapsed + dt
    if done or elapsed < Properties.StartDelay then
        self:move(0, 0)
        return
    end
    local p = self:getPosition()
    if start == nil then
        start = p
        Log.info(string.format("WalkRoute: start %.0f %.0f %.0f", p.x, p.y, p.z))
    end
    local dx, dz = p.x - start.x, p.z - start.z
    if dx * dx + dz * dz >= Properties.Distance * Properties.Distance then
        done = true
        Log.info(string.format("WalkRoute: stop %.0f %.0f %.0f", p.x, p.y, p.z))
        self:move(0, 0)
        return
    end
    self:move(1, 0)
end
