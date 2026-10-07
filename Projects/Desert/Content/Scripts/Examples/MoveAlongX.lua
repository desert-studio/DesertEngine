-- Moves the entity along world +X at a constant speed — the minimal mover the particle-space probes
-- (PART_WorldSpace / PART_LocalSpace) need: a "Simulate In World" emitter should leave its particles
-- strung out along this path, and a local-space one should carry them with it. Speed is a property so
-- the probes can state their own pace.

Properties = {
    Speed = 300.0, -- cm/s along world +X
}

function OnUpdate(dt)
    local x, y, z = self:getPosition()
    self:setPosition(x + Properties.Speed * dt, y, z)
end
