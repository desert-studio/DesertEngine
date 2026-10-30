#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/ECS/EntityValue.hpp>

namespace Desert::Libraries
{
    /// The runtime face of an entity's StreamingSourceComponent — UE's UWorldPartitionStreamingSourceComponent
    /// functions a gameplay script calls. Turning the source on or off, its range, angle and priority are the
    /// component's own fields (`entity:component("StreamingSource")`); what only the running world knows is here.
    struct StreamingSourceLibrary
    {
        REFLECT( ScriptName( "WorldStreaming" ) )

        /// UE's IsStreamingCompleted, as a count: how many cells this entity's source wants (its own shapes, as
        /// of the last streaming tick) are not resident yet — 0 is complete. A count, not a bool, because a
        /// reflected ResultStr<bool> answers as an outcome (ok, why) and would read "true" for any success. A
        /// disabled source wants nothing (0); a source enabled since the last tick has not been weighed yet and
        /// answers -1. Refused, by name, when no world runs the call, the entity has no Streaming Source, or the
        /// world is not partitioned.
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "streamingCellsPending" ) )
        static Common::ResultStr<int> StreamingCellsPending( ECS::Entity entity );
    };
} // namespace Desert::Libraries
