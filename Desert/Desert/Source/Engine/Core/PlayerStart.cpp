#include "PlayerStart.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <vector>

namespace Desert::Core
{
    namespace
    {
        glm::mat4 WorldTransformOf( const entt::registry& registry, entt::entity entity )
        {
            glm::mat4    world = registry.get<ECS::TransformComponent>( entity ).GetTransform();
            entt::entity cur   = entity;
            while ( const auto* rel = registry.try_get<ECS::RelationshipComponent>( cur ) )
            {
                if ( rel->Parent == entt::null )
                    break;
                cur = rel->Parent;
                if ( const auto* parent = registry.try_get<ECS::TransformComponent>( cur ) )
                    world = parent->GetTransform() * world;
            }
            return world;
        }

        std::string EntityName( const entt::registry& registry, entt::entity entity )
        {
            const auto* tag = registry.try_get<ECS::TagComponent>( entity );
            return tag ? tag->Tag : std::string( "<unnamed>" );
        }
    } // namespace

    Common::ResultStr<ECS::Entity> SpawnDefaultPawn( Scene& scene, const Assets::AssetManager& assets,
                                                     const PlayRequest& request )
    {
        scene.SetPlayerPawn( entt::null );
        const Assets::AssetHandle pawnHandle = scene.GetSettings().DefaultPawn;
        if ( !pawnHandle )
            return Common::MakeSuccess( ECS::Entity{} );

        const std::string level = "level '" + scene.GetSceneName() + "'";
        auto&             reg   = scene.GetRegistry();

        glm::mat4 spawnAt( 1.0f );
        if ( request.SpawnAt )
        {
            spawnAt = *request.SpawnAt;
        }
        else
        {
            std::vector<entt::entity>         entities;
            std::vector<PlayerStartCandidate> starts;
            for ( const auto entity : reg.view<ECS::PlayerStartComponent, ECS::TransformComponent>() )
            {
                entities.push_back( entity );
                starts.push_back(
                     { EntityName( reg, entity ), reg.get<ECS::PlayerStartComponent>( entity ).Data.Tag } );
            }
            const auto chosen = ChoosePlayerStart( starts, request.PlayerStartTag );
            if ( !chosen )
                return Common::MakeError<ECS::Entity>( level + " names a Default Pawn but " + chosen.GetError() );
            spawnAt = WorldTransformOf( reg, entities[chosen.GetValue()] );
        }

        const auto prefab = assets.FindByHandle<Assets::PrefabAsset>( pawnHandle );
        if ( !prefab )
            return Common::MakeError<ECS::Entity>( level + ": its Default Pawn (asset handle " +
                                      std::to_string( static_cast<uint64_t>( pawnHandle ) ) +
                                      ") is not a loaded prefab" );

        glm::vec3 scale, translation, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        glm::decompose( spawnAt, scale, rotation, translation, skew, perspective );

        auto placed = prefab->Instantiate( &scene, assets, {}, &translation );
        if ( !placed )
            return Common::MakeError<ECS::Entity>( level + ": the Default Pawn could not be spawned: " + placed.GetError() );
        ECS::Entity pawn = placed.GetValue();
        // The start's facing is the pawn's facing (UE spawns at the PlayerStart's rotation); only yaw and
        // pitch of a camera matter for Play from Here, and the prefab's own scale is kept.
        pawn.GetComponent<ECS::TransformComponent>().Rotation = glm::eulerAngles( rotation );
        scene.SetPlayerPawn( pawn.GetHandle() );
        return Common::MakeSuccess( pawn );
    }

    Common::BoolResultStr BeginPlay( Scene& scene, const Assets::AssetManager& assets, const PlayRequest& request )
    {
        const auto pawn = SpawnDefaultPawn( scene, assets, request );
        if ( !pawn )
            return Common::MakeError( pawn.GetError() );

        scene.SetPlayFromHere( request.SpawnAt.has_value() );
        if ( const auto view = scene.ResolveViewTarget(); !view )
        {
            if ( pawn.GetValue() )
                scene.DestroyEntity( pawn.GetValue() );
            scene.SetPlayerPawn( entt::null );
            scene.SetPlayFromHere( false );
            return Common::MakeError( view.GetError() );
        }
        scene.SetState( Scene::SceneState::Play );
        return BOOLSUCCESS;
    }
} // namespace Desert::Core
