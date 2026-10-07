-- Pulses the entity's material colour through the unified material protocol.
-- Attach to any mesh entity (Details -> Add Component -> Script) and press Play.
--
-- Showcases the scripting API:
--   self:setMaterialParam(name, x, y, z, w)  -- params by shader-schema name (PBR or custom)
--   self:getMaterialParam(name)              -- -> x, y, z, w
--   self:setShader(name) / self:getShader()  -- assign a surface shader ("" = PBR slots)
--   Log.info / Log.warn / Log.error          -- leveled output into the Logs panel
--
-- Edit this file while the scene is PLAYING — it hot-reloads on save.

Properties = {
    Speed = 2.0,   -- pulse frequency (radians/sec)
    UseUnlit = false, -- true: switch the entity to the Unlit shader on start
}

local t = 0.0

function OnStart()
    if Properties.UseUnlit then
        self:setShader("Unlit")
        Log.info(self:name() .. ": switched to Unlit shader")
    end
    Log.info(self:name() .. ": PulseMaterial started (speed " .. tostring(Properties.Speed) .. ")")
end

function OnUpdate(dt)
    t = t + dt * Properties.Speed

    local r = 0.5 + 0.5 * math.sin(t)
    local g = 0.5 + 0.5 * math.sin(t + 2.094)
    local b = 0.5 + 0.5 * math.sin(t + 4.188)

    -- Same param name drives the PBR tint AND the Unlit shader's Color — one protocol.
    if self:getShader() == "Unlit" then
        self:setMaterialParam("Color", r, g, b, 1.0)
    else
        self:setMaterialParam("AlbedoColor", r, g, b, 1.0)
    end
end
