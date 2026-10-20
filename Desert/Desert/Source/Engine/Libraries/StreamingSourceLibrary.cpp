#include <Engine/Libraries/StreamingSourceLibrary.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>

#include <cstdint>

namespace Desert::Libraries
{
    Common::ResultStr<bool> StreamingSourceLibrary::IsStreamingCompleted( ECS::Entity entity )
    {
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
            return Common::MakeError<bool>( "WorldStreaming.isStreamingCompleted: no world runs this call" );
        entt::registry& registry = context->World->GetRegistry();
        const auto      handle   = entity.GetHandle();
        if ( !registry.valid( handle ) )
            return Common::MakeError<bool>( "WorldStreaming.isStreamingCompleted: the entity is not alive" );
        const auto* source = registry.try_get<ECS::StreamingSourceComponent>( handle );
        if ( source == nullptr )
        {
            return Common::MakeFormattedError<bool>(
                 "WorldStreaming.isStreamingCompleted: entity {} has no Streaming Source",
                 static_cast<std::uint32_t>( handle ) );
        }
        const auto* wait = registry.ctx().find<Core::WorldStreamingWait>();
        if ( wait == nullptr )
        {
            return Common::MakeError<bool>(
                 "WorldStreaming.isStreamingCompleted: this world is not partitioned (no streaming runs)" );
        }
        if ( !source->Data.Enabled )
            return Common::MakeSuccess( true );
        const auto weighed = wait->Sources.find( static_cast<std::uint32_t>( handle ) );
        if ( weighed == wait->Sources.end() )
            return Common::MakeSuccess( false );
        return Common::MakeSuccess( weighed->second.Complete() );
    }
} // namespace Desert::Libraries
