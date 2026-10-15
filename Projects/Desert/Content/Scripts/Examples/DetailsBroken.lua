-- A script with a mistake on purpose: Details shows its error with the line and opens the code editor there.

Properties = {
    Speed = Property.Float( 2.0, { min = 0, max = 10 } ),
}

function OnUpdate( dt )
    local x = Properties.Speed * dt
    if x > 1 then
        Log.info( "fast" )
    -- the `end` of this `if` is missing
end
