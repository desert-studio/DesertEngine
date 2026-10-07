-- Candle-style light flicker via the AUTO-GENERATED component bindings: every
-- PROPERTY()-reflected component field is scriptable with zero binding code.
--
--   self:component("PointLight")   -> proxy over the reflected fields (nil if absent)
--   proxy.FieldName                -> read (numbers, bools, strings; vecs as {x,y,z})
--   proxy.FieldName = value        -> write
--   self:hasComponent("RigidBody") -> boolean
--
-- Attach to an entity that has a Point Light and press Play.

Properties = {
    BaseIntensity = 3.0,
    Flicker = 0.35, -- 0..1: how strong the flicker is
    Speed = 9.0,
}

local t = 0.0

function OnStart()
    if not self:hasComponent("PointLight") then
        Log.warn(self:name() .. ": FlickerLight needs a Point Light component")
    end
end

function OnUpdate(dt)
    local light = self:component("PointLight")
    if not light then return end

    t = t + dt * Properties.Speed
    local noise = math.sin(t) * 0.6 + math.sin(t * 2.7 + 1.3) * 0.3 + math.sin(t * 7.1) * 0.1
    light.Intensity = Properties.BaseIntensity * (1.0 + noise * Properties.Flicker)
end
