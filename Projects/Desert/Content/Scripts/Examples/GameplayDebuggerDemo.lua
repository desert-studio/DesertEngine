-- The gameplay debugger switched from a script (GameplayDebugger library; in the game the ` key and the digits
-- do the same): it shows from the first frame of Play and its Basic category turns off after
-- Properties.BasicOffAfter seconds. GDT_GameplayDebugger.desce shoots it at 0.5 s (the overlay about what the
-- view's centre looks at, Basic on) and at 2 s (Basic off: the header and the category list only).

Properties = {
    BasicOffAfter = Property.Float( 1.0 ), -- seconds of world time; under `--play` dt is a fixed 1/60 s
}

local elapsed = 0.0
local basicOff = false

function OnStart()
    GameplayDebugger.setActive( true )
end

function OnUpdate(dt)
    elapsed = elapsed + dt
    if not basicOff and elapsed >= Properties.BasicOffAfter then
        basicOff = GameplayDebugger.setCategory( "Basic", false )
    end
end
