-- Fires this entity's field once, a moment after Play starts (UE: a Field System Actor whose graph calls
-- ApplyPhysicsField on a timer). Put it on an entity with a Radial Impulse / Strain / Kill / Anchor Field
-- component; the field acts at the entity's pose, and the destructibles it breaks report OnBreak as usual.

Properties = {
    Delay = Property.Float( 1.0, { min = 0, max = 60, units = "s", tooltip = "Seconds after Play starts" } ),
}

function OnStart()
    Timer.after(Properties.Delay, function()
        local acted = Destruction.applyField(self)
        Log.info(self:name() .. ": field fired, acted on " .. tostring(acted) .. " bodies/pieces")
    end)
end
