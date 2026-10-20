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

        /// UE's IsStreamingCompleted: every cell this entity's source wants (its own shapes, as of the last
        /// streaming tick) is resident. A disabled source wants nothing and is complete; a source enabled since
        /// the last tick has not been weighed yet and is not. Refused, by name, when no world runs the call, the
        /// entity has no Streaming Source, or the world is not partitioned.
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "isStreamingCompleted" ) )
        static Common::ResultStr<bool> IsStreamingCompleted( ECS::Entity entity );
    };
} // namespace Desert::Libraries
