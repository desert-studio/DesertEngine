#include <Engine/ECS/System/DestructionLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/System/DestructionFields.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>

namespace Desert::ECS
{
    // The world is the PLAYED scene's (WorldContext::Current, set while a script runs), reached through its
    // physics system, the destruction world's one owner: null in Edit, so a field fired outside Play is refused by
    // name rather than acting on nothing.
    int DestructionLibrary::ApplyField( Entity entity )
    {
        if ( !entity || entity.GetRegistry() == nullptr )
        {
            LOG_ERROR( "[Script] Destruction.applyField: no entity" );
            return -1;
        }
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
        {
            LOG_ERROR( "[Script] Destruction.applyField: no played scene" );
            return -1;
        }
        const auto*                    physics = context->World->FindSystem<PhysicsECSSystem>();
        Destruction::DestructionWorld* world   = physics != nullptr ? physics->GetDestructionWorld() : nullptr;
        if ( world == nullptr )
        {
            LOG_ERROR(
                 "[Script] Destruction.applyField: the scene has no destruction world (fields fire in Play)" );
            return -1;
        }
        const auto fired = FireDestructionField( *entity.GetRegistry(), entity.GetHandle(), *world );
        if ( !fired )
        {
            LOG_ERROR( "[Script] Destruction.applyField: {}", fired.GetError() );
            return -1;
        }
        return static_cast<int>( fired.GetValue() );
    }
} // namespace Desert::ECS
