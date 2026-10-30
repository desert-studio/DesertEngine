-- Impact events for the VFX/Demo/Impact system's ChannelSparks emitter: writes one entry into the game's
-- VFX/Channels/Impacts data channel every Period seconds, at this entity's position with a surface Normal
-- (UE: gameplay writes a Niagara Data Channel; every system reading it spawns from the entries in the same frame).
-- An Impact placement within its Spawn from Channel MaxDistance (100 cm) answers with a spark burst at the event.
-- Positions are cm; a channel field takes a table {x, y, z}.

Properties = {
    Channel = Property.String( "Impacts", { category = "Channel", tooltip = "The VFX data channel written" } ),
    Period = Property.Float( 0.75, { min = 0.05, max = 10, units = "s", category = "Channel" } ),
    NormalX = Property.Float( 0.0, { category = "Surface" } ),
    NormalY = Property.Float( 1.0, { category = "Surface" } ),
    NormalZ = Property.Float( 0.0, { category = "Surface" } ),
}

local function impact()
    local p = self:getPosition()
    VFX.writeChannel(Properties.Channel, {
        {
            Position = { p.x, p.y, p.z },
            Normal = { Properties.NormalX, Properties.NormalY, Properties.NormalZ },
        },
    })
    Timer.after(Properties.Period, impact)
end

function OnStart()
    VFX.useChannel(Properties.Channel)
    impact()
end
