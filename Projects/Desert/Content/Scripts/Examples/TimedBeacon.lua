-- A blinking beacon: adds a point light to the entity and toggles it on a timer.
-- Attach to any mesh entity (Details -> Add Component -> Script) and press Play.
--
-- Showcases the scripting API:
--   Timer.after(seconds, fn)            -- deferred callback; re-arm inside fn to repeat
--   self:addComponent("PointLight")     -- reflection-driven components from script
--   light.Intensity / light.Color       -- reflected fields read/write directly
--   Input.wasPressed("B")               -- edge-detected key press (full keyboard)
--   self:forward() / self:distanceTo()  -- spatial helpers
--
-- Press B while playing to toggle the beacon. Edit this file while PLAYING - it hot-reloads.

Properties = {
    Period = 0.5,     -- seconds per on/off phase
    Intensity = 6.0,  -- light intensity while on
}

local lit     = false
local enabled = true

local function blink()
    if enabled then
        lit = not lit
        local light = self:component("PointLight")
        if light then
            light.Intensity = lit and Properties.Intensity or 0.0
        end
        self:setMaterialParam("EmissionFactor", lit and 1.0 or 0.0)
    end
    Timer.after(Properties.Period, blink) -- re-arm: a repeating timer is just recursion
end

function OnStart()
    local light = self:addComponent("PointLight")
    light.Color = { x = 1.0, y = 0.55, z = 0.15 }
    light.Intensity = 0.0

    Timer.after(Properties.Period, blink)
    Log.info(self:name() .. ": beacon armed (period " .. tostring(Properties.Period) .. "s)")
end

function OnUpdate(dt)
    if Input.wasPressed("B") then
        enabled = not enabled
        Log.info(self:name() .. ": beacon " .. (enabled and "enabled" or "disabled"))
        if not enabled then
            local light = self:component("PointLight")
            if light then light.Intensity = 0.0 end
            self:setMaterialParam("EmissionFactor", 0.0)
            lit = false
        end
    end

    -- Report how far the player is, once a beacon phase (cheap demo of distanceTo/forward).
    local player = World.find("Player")
    if player:valid() and Input.wasPressed("N") then
        Log.info(string.format("player is %.1fm away", self:distanceTo(player)))
    end
end
