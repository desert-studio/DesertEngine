-- The WP_Protocol plane (Scenarios/World/WP-SRC-VELOCITY.scn): flies along world +X at 250 m/s for 4 s
-- (1 km) in Play, then stops dead, so its streaming source's look-ahead stretches the loading shape ahead
-- while it flies and lets it fall back to a circle once it stands.

Properties = {
    Speed    = Property.Float( 25000.0 ), -- cm/s along world +X
    Duration = Property.Float( 4.0 ),     -- seconds of flight
}

local elapsed = 0.0

function OnUpdate(dt)
    if elapsed >= Properties.Duration then
        return
    end
    elapsed = elapsed + dt
    self:translate(vector.create(Properties.Speed * dt, 0, 0))
end
