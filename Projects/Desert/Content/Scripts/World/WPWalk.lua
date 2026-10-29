-- The WP_Protocol player (Scenarios/World/WP22-REST.scn): walks 200 m along world +X at 20 m/s in Play,
-- then stands, so the World Partition map shows the cells loading along its path and settling around it.

Properties = {
    Speed    = Property.Float( 2000.0 ), -- cm/s along world +X
    Duration = Property.Float( 10.0 ),   -- seconds of walking; 10 s x 20 m/s = 200 m
}

local elapsed = 0.0

function OnUpdate(dt)
    if elapsed >= Properties.Duration then
        return
    end
    elapsed = elapsed + dt
    self:translate(vector.create(Properties.Speed * dt, 0, 0))
end
