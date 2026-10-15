-- Every kind of field a script can show in Details, in one place: the Details panel draws each declaration
-- below the way it draws a C++ PROPERTY (range slider, units, colour, vector, asset slot with its picture,
-- entity slot with the eyedropper, enum, arrays, categories, EditCondition, Advanced). In Play the entity
-- swings along world X by Amplitude at Speed: change either in Details and the swing changes on the next frame.

Properties = {
    Speed = Property.Float( 2.0, { min = 0, max = 10, units = "rad/s", category = "Movement",
                                   tooltip = "How fast the entity swings along X" } ),
    Amplitude = Property.Float( 150.0, { min = 0, max = 1000, units = "cm", category = "Movement" } ),
    Paused = Property.Bool( false, { category = "Movement", tooltip = "Hold the swing where it is" } ),
    Laps = Property.Int( 3, { min = 0, max = 20, category = "Movement", displayName = "Lap Count" } ),
    Offset = Property.Vector( vector.create( 0, 0, 0 ), { units = "cm", category = "Movement" } ),

    Label = Property.String( "Showcase", { category = "Look" } ),
    Tint = Property.Color( vector.create( 1.0, 0.55, 0.1 ), { category = "Look" } ),
    UseTint = Property.Bool( true, { category = "Look" } ),
    TintStrength = Property.Float( 0.8, { min = 0, max = 1, category = "Look", editCondition = "UseTint" } ),
    Mode = Property.Enum( { "Swing", "Bob", "Still" }, "Swing", { category = "Look" } ),
    Mesh = Property.Asset( "StaticMesh", { category = "Look", tooltip = "A mesh to show" } ),

    Target = Property.Entity( { category = "Targets", tooltip = "An entity to face" } ),
    Waypoints = Property.Array( Property.Vector( vector.create( 0, 0, 0 ) ), {}, { category = "Targets" } ),
    Weights = Property.Array( Property.Float( 1.0 ), { 1.0, 0.5 }, { category = "Targets" } ),

    DebugLog = Property.Bool( false, { advanced = true, category = "Movement" } ),
}

CallInEditor = { "Respawn" }

local t = 0.0
local baseX, baseY, baseZ = 0.0, 0.0, 0.0

function Respawn()
    t = 0.0
    Log.info( self:name() .. ": respawned" )
end

function OnStart()
    local p = self:getPosition()
    baseX, baseY, baseZ = p.x, p.y, p.z
end

function OnUpdate( dt )
    if Properties.Paused or Properties.Mode == "Still" then
        return
    end
    t = t + dt * Properties.Speed
    local swing = math.sin( t ) * Properties.Amplitude
    local o = Properties.Offset
    if Properties.Mode == "Bob" then
        self:setPosition( vector.create( baseX + o.x, baseY + o.y + swing, baseZ + o.z ) )
    else
        self:setPosition( vector.create( baseX + o.x + swing, baseY + o.y, baseZ + o.z ) )
    end
    if Properties.DebugLog then
        Log.info( string.format( "%s: swing %.1f", Properties.Label, swing ) )
    end
end
