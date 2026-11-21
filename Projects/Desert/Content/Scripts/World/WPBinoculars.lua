-- WP-FAR-9 (Scenarios/World/WP-FAR-9.scn): binoculars as a streaming source provider with no entity of its own
-- (UE IWorldPartitionStreamingSourceProvider). For HoldSeconds the player's look is a narrow, long sector that
-- streams the Main grid's real cells far ahead; then the source is removed and those cells fall back to HLOD.

Properties = {
    HoldSeconds = Property.Float( 6.0 ),      -- seconds the binoculars are up
    Yaw         = Property.Float( 180.0 ),    -- degrees, counter-clockwise from -Z seen from above; 180 = +Z
    Radius      = Property.Float( 200000.0 ), -- cm the binoculars reach (2 km)
    SectorAngle = Property.Float( 10.0 ),     -- degrees across
}

local held = 0.0
local up = false

function OnUpdate(dt)
    if held >= Properties.HoldSeconds then
        if up then
            WorldPartition.removeSource("Binoculars")
            up = false
        end
        return
    end
    held = held + dt
    WorldPartition.provideSource("Binoculars", {
        Location = self:getPosition(),
        Yaw = Properties.Yaw,
        Shapes = { { Radius = Properties.Radius, IsSector = true, SectorAngle = Properties.SectorAngle } },
        TargetGrids = { "Main" },
    })
    up = true
end
