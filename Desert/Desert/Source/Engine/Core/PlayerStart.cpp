#include "PlayerStart.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Core/PawnBodyRules.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/Runtime/Factory/PrefabFactory.hpp>
#include <Common/Json/Document.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <format>
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
            return tag != nullptr ? tag->Tag : std::string( "<unnamed>" );
        }

        // The level's Default Pawn prefab, loaded: the ONE statement of "is not a loaded prefab", for Play
        // and for the PlayerStart's capsule alike.
        Common::ResultStr<Assets::Asset<Assets::PrefabAsset>>
        DefaultPawnPrefab( const Scene& scene, const Assets::AssetManager& assets, Assets::AssetHandle pawnHandle )
        {
            auto prefab = assets.FindByHandle<Assets::PrefabAsset>( pawnHandle );
            if ( !prefab || !prefab->IsReadyForUse() )
                return Common::MakeFormattedError<Assets::Asset<Assets::PrefabAsset>>(
                     "level '{}': its Default Pawn (asset handle {}) is not a loaded prefab", scene.GetSceneName(),
                     static_cast<uint64_t>( pawnHandle ) );
            return Common::MakeSuccess( std::move( prefab ) );
        }
    } // namespace

    Common::ResultStr<ECS::Entity> SpawnDefaultPawn( Scene& scene, const Assets::AssetManager& assets,
                                                     const PlayRequest& request )
    {
        scene.SetPlayerPawn( entt::null );
        const Assets::AssetHandle pawnHandle = scene.GetSettings().DefaultPawn;
        if ( pawnHandle == 0 )
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

        const auto found = DefaultPawnPrefab( scene, assets, pawnHandle );
        if ( !found )
            return Common::MakeError<ECS::Entity>( found.GetError() );
        const Assets::Asset<Assets::PrefabAsset>& prefab = found.GetValue();

        glm::vec3 scale;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::quat rotation;
        glm::vec4 perspective;
        glm::decompose( spawnAt, scale, rotation, translation, skew, perspective );

        auto placed = prefab->Instantiate( &scene, assets, {}, &translation );
        if ( !placed )
            return Common::MakeError<ECS::Entity>(
                 level + ": the Default Pawn could not be spawned: " + placed.GetError() );
        ECS::Entity pawn = placed.GetValue();
        // The start's facing is the pawn's facing (UE spawns at the PlayerStart's rotation); only yaw and
        // pitch of a camera matter for Play from Here, and the prefab's own scale is kept.
        pawn.GetComponent<ECS::TransformComponent>().Rotation = glm::eulerAngles( rotation );
        // The player is where the world must exist: UE's player controller is a streaming source by default. A
        // prefab that authored its own (a range, a priority, or Enabled off for a pawn that should not stream)
        // keeps it.
        if ( !pawn.HasComponent<ECS::StreamingSourceComponent>() )
            pawn.AddComponent<ECS::StreamingSourceComponent>();
        scene.SetPlayerPawn( pawn.GetHandle() );
        return Common::MakeSuccess( pawn );
    }

    Common::ResultStr<std::optional<PawnCapsule>> DefaultPawnCapsule( const Scene&                scene,
                                                                      const Assets::AssetManager& assets )
    {
        const Assets::AssetHandle pawnHandle = scene.GetSettings().DefaultPawn;
        if ( pawnHandle == 0 )
            return Common::MakeSuccess( std::optional<PawnCapsule>{} );

        const auto prefab = DefaultPawnPrefab( scene, assets, pawnHandle );
        if ( !prefab )
            return Common::MakeError<std::optional<PawnCapsule>>( prefab.GetError() );

        const auto block = PawnControllerBlock(
             prefab.GetValue()->GetEntities(),
             [&assets]( const std::string& path ) -> Common::ResultStr<const std::vector<Assets::EntityData>*>
             {
                 const auto resolved = Runtime::Factory::PrefabFactory::ResolveNested( assets, path );
                 if ( !resolved )
                     return Common::MakeError<const std::vector<Assets::EntityData>*>( resolved.GetError() );
                 return Common::MakeSuccess( &resolved.GetValue()->GetEntities() );
             } );
        if ( !block )
            return Common::MakeFormattedError<std::optional<PawnCapsule>>(
                 "level '{}': its Default Pawn: {}", scene.GetSceneName(), block.GetError() );
        const auto& pawnBlock = block.GetValue();
        if ( !pawnBlock.has_value() )
            return Common::MakeSuccess( std::optional<PawnCapsule>{} );

        const auto* type = Reflection::ReflectionRegistry::Get().Find( "CharacterControllerData" );
        if ( type == nullptr )
            return Common::MakeError<std::optional<PawnCapsule>>( "CharacterControllerData is not reflected" );
        ECS::CharacterControllerData data{};
        Common::Json::Issues         issues;
        Reflection::DeserializeReflected(
             *type, &data,
             Common::Json::Root( pawnBlock.value(), Common::Json::Path().Key( "CharacterController" ) ), issues );
        if ( !issues.empty() )
            return Common::MakeFormattedError<std::optional<PawnCapsule>>(
                 "level '{}': its Default Pawn's CharacterController block is malformed ({} issue(s))",
                 scene.GetSceneName(), issues.size() );
        return Common::MakeSuccess( std::optional<PawnCapsule>( PawnCapsule{ data.Radius, data.Height } ) );
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
