-- The WP_Protocol far flyer 9000003 (Scenarios/World/SCOPE-FAR-HLOD.scn): in Play it holds over the city centre,
-- flies along world +X to the mid band, holds, on to the far band, holds, and flies back to the centre, where it
-- stays. Its streaming source walks the camera's distance through every HLOD band of the city:
--   centre (0 m)     - Main cells within 200 m Activated, all 16 HLOD_Main cells (512 m) Activated;
--   mid    (1800 m)  - no Main cell, the 4 HLOD_Main cells of column X=1 Activated, the other 12 unloaded;
--   far    (5000 m)  - no Main cell, no HLOD_Main cell: the 16 always-loaded HLOD_Main_Far HLODs stand in.
-- Each hold is 30 s of simulated time, so a scenario's `advance` lands inside it whatever Play's start cost.
-- AlwaysLoaded (the entity file), so flying away from its own Main cell never unloads it.

Properties = {
    Speed = Property.Float( 25000.0 ),  -- cm/s along world X
    Mid   = Property.Float( 180000.0 ), -- cm from the start: the mid band
    Far   = Property.Float( 500000.0 ), -- cm from the start: the far band
    Hold  = Property.Float( 30.0 ),     -- seconds held at the start, at Mid and at Far
}

local travelled = 0.0 -- cm along +X from the start
local leg       = 1   -- index into the legs below
local held      = 0.0
-- Each leg: hold Hold seconds, then fly to `to` (cm from the start); past the last leg it holds forever. Built on
-- the first update, once the Properties carry the entity's values.
local plan      = nil

function OnUpdate(dt)
    if plan == nil then
        plan = { Properties.Mid, Properties.Far, 0.0 }
    end
    local to = plan[leg]
    if to == nil then
        return
    end
    if held < Properties.Hold then
        held = held + dt
        return
    end
    local step      = Properties.Speed * dt
    local remaining = to - travelled
    if math.abs(remaining) <= step then
        self:translate(vector.create(remaining, 0, 0))
        travelled = to
        leg       = leg + 1
        held      = 0.0
        return
    end
    local move = remaining > 0 and step or -step
    self:translate(vector.create(move, 0, 0))
    travelled = travelled + move
end
