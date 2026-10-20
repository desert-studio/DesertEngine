#include <Engine/Libraries/StreamingSourceLibrary.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>

#include <cstdint>

namespace Desert::Libraries
{
    Common::ResultStr<int> StreamingSourceLibrary::StreamingCellsPending( ECS::Entity entity )
    {
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
            return Common::MakeError<int>( "WorldStreaming.streamingCellsPending: no world runs this call" );
        entt::registry& registry = context->World->GetRegistry();
        const auto      handle   = entity.GetHandle();
        if ( !registry.valid( handle ) )
            return Common::MakeError<int>( "WorldStreaming.streamingCellsPending: the entity is not alive" );
        const auto* source = registry.try_get<ECS::StreamingSourceComponent>( handle );
        if ( source == nullptr )
        {
            return Common::MakeFormattedError<int>(
                 "WorldStreaming.streamingCellsPending: entity {} has no Streaming Source",
                 static_cast<std::uint32_t>( handle ) );
        }
        const auto* wait = registry.try_ctx<Core::WorldStreamingWait>();
        if ( wait == nullptr )
        {
            return Common::MakeError<int>(
                 "WorldStreaming.streamingCellsPending: this world is not partitioned (no streaming runs)" );
        }
        if ( !source->Data.Enabled )
            return Common::MakeSuccess( 0 );
        const auto weighed = wait->Sources.find( static_cast<std::uint32_t>( handle ) );
        if ( weighed == wait->Sources.end() )
            return Common::MakeSuccess( -1 );
        const Core::Rules::SourceCompletion& c = weighed->second;
        return Common::MakeSuccess( static_cast<int>( c.Wanted - c.Activated - c.Failed ) );
    }
} // namespace Desert::Libraries
