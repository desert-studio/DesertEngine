#include <Engine/Libraries/GameModeLibrary.hpp>

#include <Engine/Core/GameMode.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>

#include <optional>

namespace Desert::Libraries
{
    namespace
    {
        Core::Scene* PlayedScene()
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            return context != nullptr ? context->World : nullptr;
        }

        ECS::Entity EntityOrNil( Core::Scene* scene, entt::entity entity )
        {
            if ( scene == nullptr || entity == entt::null )
                return {};
            return ECS::Entity( entity, scene->GetRegistry() );
        }
    } // namespace

    Common::BoolResultStr GameModeLibrary::Kill( ECS::Entity pawn )
    {
        Core::Scene* scene = PlayedScene();
        if ( scene == nullptr )
            return Common::MakeError<bool>( "no world is being played" );
        return scene->GetGameMode().Kill( *scene, pawn.GetHandle() );
    }

    ECS::Entity GameModeLibrary::Pawn()
    {
        Core::Scene* scene = PlayedScene();
        return EntityOrNil( scene, scene != nullptr ? scene->GetPlayerPawn() : entt::null );
    }

    ECS::Entity GameModeLibrary::Controller()
    {
        Core::Scene* scene = PlayedScene();
        return EntityOrNil( scene, scene != nullptr ? scene->GetPlayerController() : entt::null );
    }

    Reflection::Value GameModeLibrary::RespawnIn()
    {
        const Core::Scene* scene = PlayedScene();
        if ( scene == nullptr )
            return {};
        const std::optional<float> remaining = scene->GetGameMode().RespawnRemaining();
        return remaining ? Reflection::Value::Double( *remaining ) : Reflection::Value();
    }
} // namespace Desert::Libraries
