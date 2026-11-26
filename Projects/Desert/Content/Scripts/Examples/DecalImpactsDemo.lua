-- Impact decals (World.spawnDecal, UE SpawnDecalAtLocation; NG-DECALS-MESH): at the start of Play this leaves
-- Properties.Count impacts, half on a wall plane (normal +Z) and half on the ground (normal +Y), spread over a grid
-- around this entity. The first quarter spawns with a short Lifetime, so by 3 s they have faded out (UE SetFadeOut)
-- while the rest stay. No entity per decal: every one is a record of the world's ring (Effects.DecalImpactPoolSize).
-- Scenarios/Render/NG-DECALS.scn shoots it on FIELD_Barn.

Properties = {
    Count = Property.Int( 500 ),
    Material = Property.Asset( "Material" ), -- a Deferred Decal material; empty = the engine's default decal
    WallZ = Property.Float( -300.0 ),        -- the wall plane, cm from this entity along -Z
}

function OnStart()
    local origin = self:getPosition()
    local half = Properties.Count // 2
    local failed = 0
    for i = 0, Properties.Count - 1 do
        local onWall = i < half
        local k = onWall and i or ( i - half )
        local col = k % 25
        local row = k // 25
        local at, normal
        if onWall then
            at = origin + vector.create( ( col - 12 ) * 20, 20 + row * 20, Properties.WallZ )
            normal = vector.create( 0, 0, 1 )
        else
            at = origin + vector.create( ( col - 12 ) * 20, 0, Properties.WallZ + 40 + row * 20 )
            normal = vector.create( 0, 1, 0 )
        end
        local short = ( k % 4 ) == 0
        local ok = World.spawnDecal( {
            Material = Properties.Material,
            At = at,
            Normal = normal,
            Size = vector.create( 6, 6, 4 ),
            Roll = ( i * 37 ) % 360,
            Lifetime = short and 2.0 or 0.0,
            FadeDuration = short and 1.0 or 0.0,
        } )
        if not ok then
            failed = failed + 1
        end
    end
    log( "DecalImpactsDemo: spawned", Properties.Count - failed, "refused", failed )
end
