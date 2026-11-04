-- Gameplay debug drawing (DebugDraw library, UE's DrawDebug* nodes): once at the start of Play this draws a
-- line through the cube, a box around it, a sphere half sunk into the ground, a thick arrow, a label and three
-- 12-pixel points (pure red, green, blue; a fourth at the cube's centre, hidden by it), all for
-- Properties.Duration seconds of world time. They are drawn over the tonemapped picture, so each shows its
-- colour as given. Scenarios/Editor/DBG-DRAW.scn shoots it at 0.5 s (everything drawn, the parts inside the
-- cube and under the ground hidden by the depth test) and at 3 s (all gone).

Properties = {
    Duration = Property.Float( 2.0 ), -- seconds of world time every shape lives
}

function OnStart()
    local d = Properties.Duration
    DebugDraw.line( vector.create( -250, 50, 150 ), vector.create( 250, 50, 150 ), vector.create( 1, 0.1, 0.1 ), d )
    DebugDraw.box( vector.create( 0, 50, 150 ), vector.create( 70, 70, 70 ), vector.create( 0.1, 1, 0.2 ), d )
    DebugDraw.sphere( vector.create( -170, 0, 150 ), 60, vector.create( 0.2, 0.5, 1 ), d, 16 )
    DebugDraw.arrow( vector.create( 120, 0, 150 ), vector.create( 200, 120, 150 ), 30, vector.create( 1, 0.8, 0.1 ), d,
        3 )
    DebugDraw.point( vector.create( -60, 220, 150 ), 12, vector.create( 1, 0, 0 ), d )
    DebugDraw.point( vector.create( 0, 220, 150 ), 12, vector.create( 0, 1, 0 ), d )
    DebugDraw.point( vector.create( 60, 220, 150 ), 12, vector.create( 0, 0, 1 ), d )
    DebugDraw.point( vector.create( 0, 50, 150 ), 12, vector.create( 1, 0, 1 ), d )
    DebugDraw.text( vector.create( -60, 150, 150 ), "DebugDraw: 2 s", vector.create( 1, 1, 1 ), d, 1.5 )
end
