#include "CloudSkyCapture.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/PlayKeptChanges.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <format>

namespace Desert::Editor
{
    namespace
    {
        // The one edit, applied to the play world now and to its authored counterpart at Stop.
        void Fold( ECS::VolumetricCloudComponent& layer, const glm::vec3& captured )
        {
            layer.Data.WindOffset = captured;
            layer.DriftOffset     = glm::vec3( 0.0f );
        }
    } // namespace

    Common::BoolResultStr CaptureCurrentSky( const ::Desert::Core::Scene& scene, ::Desert::ECS::Entity& layer )
    {
        const std::string& name = layer.GetComponent<ECS::TagComponent>().Tag;
        if ( !layer.HasComponent<ECS::VolumetricCloudComponent>() )
            return Common::MakeError<bool>(
                 std::format( "Capture current sky: '{}' has no Volumetric Cloud component", name ) );

        auto& cloud = layer.GetComponent<ECS::VolumetricCloudComponent>();
        if ( cloud.DriftOffset == glm::vec3( 0.0f ) )
            return Common::MakeError<bool>( std::format(
                 "Capture current sky: the clouds on '{}' have not drifted from their Wind Offset — the sky "
                 "moves only while the game runs (Play)",
                 name ) );

        const glm::vec3    captured = cloud.Data.WindOffset + cloud.DriftOffset;
        const Common::UUID uuid     = layer.GetComponent<ECS::UUIDComponent>().UUID;
        Commands::MutateEntityUndoable( uuid, [&cloud, captured] { Fold( cloud, captured ); } );

        if ( scene.GetState() != ::Desert::Core::Scene::SceneState::Edit )
        {
            PlayKeptChanges::Keep( uuid, "Capture current sky",
                                   [captured]( ECS::Entity& authored )
                                   {
                                       if ( authored.HasComponent<ECS::VolumetricCloudComponent>() )
                                           Fold( authored.GetComponent<ECS::VolumetricCloudComponent>(),
                                                 captured );
                                   } );
        }

        LOG_INFO( "[Clouds] captured the sky on '{}': Wind Offset = ({:.1f}, {:.1f}, {:.1f}) cm", name, captured.x,
                  captured.y, captured.z );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor
