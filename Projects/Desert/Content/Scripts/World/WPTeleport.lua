-- WP-SCRIPT-API (Scenarios/World/WP-SCRIPT-API.scn): a teleport that streams its destination before it moves the
-- player. A query loads nothing (UE FWorldPartitionStreamingQuerySource), so the destination's own Streaming Source
-- (entity tagged WPTeleportTarget) is enabled first, then the script waits on the query and moves the pawn.

local P = vector.create( 70000, 0, 0 )
local query = { Location = P, Radius = 5000 }

function OnStart()
    World.find( "WPTeleportTarget" ):component( "StreamingSource" ).Enabled = true
    print( "pending before the first tick: " .. WorldPartition.streamingPending( query, "Activated" ) )
    coroutine.wrap( function()
        -- The yielding wait: the coroutine sleeps until the destination streamed.
        local co = coroutine.running()
        WorldPartition.waitUntilStreamed( query, "Activated", function( ms ) coroutine.resume( co, ms ) end )
        local ms = coroutine.yield()
        print( "teleport after " .. ms .. " ms" )
        -- The landscape at P is resident now: stand the pawn on it.
        local ground = P + vector.create( 0, 3000, 0 )
        local hit = World.raycast( P + vector.create( 0, 100000, 0 ), vector.create( 0, -1, 0 ) )
        if hit.bBlockingHit then
            ground = hit.Location + vector.create( 0, 200, 0 )
        end
        gameMode.pawn():setPosition( ground )
        World.find( "WPTeleportTarget" ):component( "StreamingSource" ).Enabled = false
        for _, line in WorldPartition.cellStatesAt( P ) do
            print( line )
        end
    end )()
end

function OnWorldPartitionCellActivated( cell )
    print( "activated " .. cell )
end

function OnWorldPartitionCellUnloaded( cell )
    print( "unloaded " .. cell )
end
