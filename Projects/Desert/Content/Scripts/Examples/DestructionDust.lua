-- Dust for a destructible: put this script on the entity with the Destructible component, tick its Notify
-- Breaks / Notify Collisions / Notify Removals, and place a VFX entity playing VFX/DestructionDust.dfx. The engine
-- only reports the destructible's news (OnBreak / OnCollision / OnRemoved, UE OnChaosBreakEvent /
-- OnChaosPhysicsCollision / OnChaosRemovalEvent); this script decides they raise dust, and writes them into the
-- game's own VFX data channels (VFX/Channels/BreakDust, ImpactDust, SettleDust), which the dust system's
-- emitters spawn from in the same frame (UE: the actor's graph writes a Niagara Data Channel).
-- Positions arrive as vectors (p.x, p.y, p.z) in cm; a channel field takes a table {x, y, z}.

Properties = {
    BreakChannel = Property.String( "BreakDust", { category = "Channels", tooltip = "Written once per break" } ),
    ImpactChannel = Property.String( "ImpactDust", { category = "Channels", tooltip = "Written once per collision" } ),
    SettleChannel = Property.String( "SettleDust", { category = "Channels", tooltip = "Written once per removal" } ),
    -- kg*cm/s; quieter contacts raise no dust (on top of the component's own minimum)
    MinImpulse = Property.Float( 0.0, { min = 0, max = 100000, units = "kg*cm/s", category = "Dust" } ),
}

local function xyz(v)
    return { v.x, v.y, v.z }
end

function OnStart()
    VFX.useChannel(Properties.BreakChannel)
    VFX.useChannel(Properties.ImpactChannel)
    VFX.useChannel(Properties.SettleChannel)
end

function OnBreak(node, position, velocity, massKg, pieceCount)
    VFX.writeChannel(Properties.BreakChannel, {
        {
            Position = xyz(position),
            Velocity = xyz(velocity),
            Speed = math.sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z),
            MassKg = massKg,
            PieceCount = pieceCount,
        },
    })
end

function OnCollision(node, position, normal, impulse, velocity, massKg)
    if impulse < Properties.MinImpulse then
        return
    end
    VFX.writeChannel(Properties.ImpactChannel, {
        {
            Position = xyz(position),
            Normal = xyz(normal),
            Impulse = impulse,
            Velocity = xyz(velocity),
            MassKg = massKg,
        },
    })
end

function OnRemoved(node, position, massKg, pieceCount)
    VFX.writeChannel(Properties.SettleChannel, {
        { Position = xyz(position), MassKg = massKg, PieceCount = pieceCount },
    })
end
