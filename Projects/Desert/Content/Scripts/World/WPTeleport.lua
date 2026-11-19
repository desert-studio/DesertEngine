-- WP-SCRIPT-API (Scenarios/World/WP-SCRIPT-API.scn): a teleport that streams its destination before it moves the
-- player. A query loads nothing (UE FWorldPartitionStreamingQuerySource), so the destination's own Streaming Source
-- (entity tagged WPTeleportTarget) is enabled first, then the script waits on the query and moves the pawn.

local P = vector.create( 70000, 0, 0 )
-- No Radius: the grids' own loading ranges around P (UE bUseGridLoadingRange), the cells the player needs there.
local query = { Location = P }

function OnStart()
    World.find( "WPTeleportTarget" ):component( "StreamingSource" ).Enabled = true
    print( "pending before the first tick: " .. WorldPartition.streamingPending( query, "Activated" ) )
    coroutine.wrap( function()
        -- The yielding wait: the coroutine sleeps until the destination streamed.
        local co = coroutine.running()
        -- resume returns an error instead of raising it: raised here, the wait's callback error reaches the log.
        WorldPartition.waitUntilStreamed( query, "Activated", function( ms )
            local ok, err = coroutine.resume( co, ms )
            if not ok then
                error( err )
            end
        end )
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
        local states = WorldPartition.cellStatesAt( P )
        print( "cellStatesAt(P): " .. #states .. " cell(s)" )
        for _, line in states do
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
