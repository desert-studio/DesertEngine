-- The main menu's camera: one slow circle around the dune field, always looking at its centre.
Properties = {
    CentreX = 0.0, CentreY = 600.0, CentreZ = 0.0, -- cm: the point the camera circles and looks at
    Radius  = 16000.0,                              -- cm
    Height  = 2600.0,                               -- cm above the centre
    Period  = 240.0,                                -- seconds for one full circle
}

local angle = 0.0

local function Place()
    local x = Properties.CentreX + Properties.Radius * math.sin( angle )
    local z = Properties.CentreZ + Properties.Radius * math.cos( angle )
    self:setPosition( vector.create( x, Properties.CentreY + Properties.Height, z ) )
    local pitch = -math.atan( Properties.Height, Properties.Radius )
    self:setRotation( vector.create( pitch, angle, 0.0 ) )
end

function OnStart()
    Place()
end

function OnUpdate( dt )
    angle = ( angle + dt * 2.0 * math.pi / Properties.Period ) % ( 2.0 * math.pi )
    Place()
end
