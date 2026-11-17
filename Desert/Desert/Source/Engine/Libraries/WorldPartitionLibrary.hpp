#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Libraries
{
    /// THE RUNNING WORLD PARTITION, AS GAMEPLAY ASKS IT - UE's UWorldPartitionSubsystem script face
    /// (IsStreamingCompleted, IsAllStreamingCompleted, a cell's GetCurrentState, OnStreamingStateUpdated) and the
    /// latent wait a teleport or a cinematic is built on.
    ///
    /// A QUERY IS NOT A SOURCE (UE FWorldPartitionStreamingQuerySource). It is weighed by the same rule that
    /// decides what a streaming source wants (Rules::QueryStreamingCells), against the residency as it stands -
    /// and it changes nothing that loads: a query at a place no source reaches stays pending. To wait for a
    /// destination, put a Streaming Source there (an entity's StreamingSource component, enabled by the script)
    /// and query it; the query answers when what the source streams has arrived.
    ///
    /// A query is a table - or a list of them, all of which must be complete:
    ///   Location - vector, cm (required unless SpatialQuery is false; Y is the altitude, as a source's).
    ///   Radius - cm; stating it means the query's own range (UE bUseGridLoadingRange off).
    ///   UseGridLoadingRange - true: each grid's LoadingRange (the default when no Radius is stated); stating it
    ///     true beside a Radius is refused, not guessed.
    ///   Yaw - degrees the query faces, counter-clockwise from -Z seen from above (its Shapes' frame).
    ///   Shapes - list of {Radius | UseGridLoadingRange, LoadingRangeScale, IsSector, SectorAngle, Location,
    ///     Yaw} in the query's frame (UE FStreamingSourceShape); none = one circle.
    ///   TargetGrids - list of grid names (UE TargetGrids); none = every grid; a name no grid has is refused.
    ///   SpatialQuery - false: every cell of the target grids, wherever it is (UE bSpatialQuery).
    /// The state is "Loaded" (read, entities or not) or "Activated" (entities in the world) - UE's
    /// EWorldPartitionRuntimeCellState. A cell whose read FAILED counts as arrived, as it does for the loading
    /// overlay (WorldPartitionStreamingPerformance.hpp): it is logged and retried, and waiting on it would lock a
    /// game behind a missing file. Before the first streaming tick nothing has arrived and the answer says so -
    /// there is no "not weighed yet".
    struct WorldPartitionLibrary
    {
        REFLECT( ScriptName( "WorldPartition" ) )

        /// UE IsStreamingCompleted, as a count: the cells @p queries want that have not reached @p state - 0 is
        /// complete. A count, as WorldStreaming.streamingCellsPending answers, because a reflected
        /// ResultStr<bool> reads true for any success.
        FUNCTION(
             ScriptCallable, ScriptName( "streamingPending" ),
             Tooltip( "How many cells the query (a table or a list of them) wants that have not reached the "
                      "state (\"Loaded\" or \"Activated\"); 0 = streaming completed. Refused by name when the "
                      "world is not partitioned or the query is malformed." ) )
        static Common::ResultStr<int> StreamingPending( const Reflection::Value& queries,
                                                        const std::string&       state );

        /// UE IsAllStreamingCompleted: the cells every enabled streaming source of the world wants (its shapes, as
        /// the residency weighs them now) that are not Activated yet; 0 = the world around every source arrived.
        FUNCTION( ScriptCallable, ScriptName( "allStreamingPending" ),
                  Tooltip( "How many cells the world's streaming sources want that are not Activated yet; 0 = all "
                           "streaming completed." ) )
        static Common::ResultStr<int> AllStreamingPending();

        /// THE LATENT WAIT (UE's latent action on IsStreamingCompleted): @p done(ms) is called once, on the first
        /// played frame whose residency completes the query, with the milliseconds since this call - and the log
        /// says "streaming completed for query in N ms". Never on this very call: a query complete already
        /// answers on the next frame, as UE's latent action does. A wait belongs to the script that made it: a
        /// reload or the entity's destruction drops it unfired, as a timer's callback. The YIELDING form is the
        /// script's own coroutine, resumed from @p done (the engine calls it outside the coroutine):
        ///   coroutine.wrap(function()
        ///     local co = coroutine.running()
        ///     WorldPartition.waitUntilStreamed(query, "Activated", function(ms) coroutine.resume(co, ms) end)
        ///     local ms = coroutine.yield() -- the teleport / the cinematic continues here
        ///   end)()
        FUNCTION( ScriptCallable, ScriptName( "waitUntilStreamed" ),
                  Tooltip( "Calls the function (with the milliseconds waited) once the query (a table or a list) "
                           "reaches the state (\"Loaded\" or \"Activated\"). The query loads nothing itself: wait "
                           "where a streaming source streams." ) )
        static Common::BoolResultStr WaitUntilStreamed( const Reflection::Value& queries, const std::string& state,
                                                        const Reflection::Callable& done );

        /// UE UWorldPartitionRuntimeCell::GetCurrentState for every cell whose square holds @p location (X, Z),
        /// coarsest level last: "<cell>: <Unloaded|Loading|Loaded|Activated|Failed>", the cell as the streaming
        /// log names it. Empty where no planned cell holds the point.
        FUNCTION( ScriptCallable, ScriptName( "cellStatesAt" ),
                  Tooltip( "The streaming state of every cell holding the point, as \"<cell>: <state>\"." ) )
        static Common::ResultStr<std::vector<std::string>> CellStatesAt( const glm::vec3& location );

        /// Not script-callable: the host calls it once a played frame (ScriptEngine::DeliverWorldPartitionEvents)
        /// to fire the waits whose query completed, in the order they were made.
        static void FireCompletedWaits( Core::Scene& scene );
    };
} // namespace Desert::Libraries
