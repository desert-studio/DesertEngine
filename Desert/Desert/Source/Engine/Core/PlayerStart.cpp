#include "PlayerStart.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Core/GameMode.hpp>
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

    Common::ResultStr<glm::mat4> PlayerStartTransform( const Scene& scene, std::string_view tag )
    {
        const auto&                       reg = scene.GetRegistry();
        std::vector<entt::entity>         entities;
        std::vector<PlayerStartCandidate> starts;
        for ( const auto entity : reg.view<const ECS::PlayerStartComponent, const ECS::TransformComponent>() )
        {
            entities.push_back( entity );
            starts.push_back(
                 { EntityName( reg, entity ), reg.get<ECS::PlayerStartComponent>( entity ).Data.Tag } );
        }
        const auto chosen = ChoosePlayerStart( starts, tag );
        if ( !chosen )
            return Common::MakeError<glm::mat4>( chosen.GetError() );
        return Common::MakeSuccess( WorldTransformOf( reg, entities[chosen.GetValue()] ) );
    }

    Common::ResultStr<ECS::Entity> SpawnPawnPrefabAt( Scene& scene, const Assets::AssetManager& assets,
                                                      const glm::mat4& spawnAt )
    {
        const auto found = DefaultPawnPrefab( scene, assets, scene.GetSettings().DefaultPawn );
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
                 "level '" + scene.GetSceneName() +
                 "': the Default Pawn could not be spawned: " + placed.GetError() );
        ECS::Entity pawn = placed.GetValue();
        // The start's facing is the pawn's facing (UE spawns at the PlayerStart's rotation); only yaw and
        // pitch of a camera matter for Play from Here, and the prefab's own scale is kept.
        pawn.GetComponent<ECS::TransformComponent>().Rotation = glm::eulerAngles( rotation );
        // The player is where the world must exist: UE's player controller is a streaming source by default. A
        // prefab that authored its own (a range, a priority, or Enabled off for a pawn that should not stream)
        // keeps it.
        if ( !pawn.HasComponent<ECS::StreamingSourceComponent>() )
            pawn.AddComponent<ECS::StreamingSourceComponent>();
        return Common::MakeSuccess( pawn );
    }

    Common::ResultStr<ECS::Entity> SpawnDefaultPawn( Scene& scene, const Assets::AssetManager& assets,
                                                     const PlayRequest& request )
    {
        scene.SetPlayerPawn( entt::null );
        if ( scene.GetSettings().DefaultPawn == 0 )
            return Common::MakeSuccess( ECS::Entity{} );

        glm::mat4 spawnAt( 1.0f );
        if ( request.SpawnAt )
        {
            spawnAt = *request.SpawnAt;
        }
        else
        {
            const auto start = PlayerStartTransform( scene, request.PlayerStartTag );
            if ( !start )
                return Common::MakeError<ECS::Entity>( "level '" + scene.GetSceneName() +
                                                       "' names a Default Pawn but " + start.GetError() );
            spawnAt = start.GetValue();
        }

        auto spawned = SpawnPawnPrefabAt( scene, assets, spawnAt );
        if ( !spawned )
            return spawned;
        ECS::Entity pawn = spawned.GetValue();
        scene.SetPlayerPawn( pawn.GetHandle() );
        return Common::MakeSuccess( pawn );
    }

    Common::ResultStr<ECS::Entity> SpawnPlayerController( Scene& scene, const Assets::AssetManager& assets )
    {
        scene.SetPlayerController( entt::null );
        const Assets::AssetHandle handle = scene.GetSettings().PlayerController;
        if ( handle == 0 )
            return Common::MakeSuccess( ECS::Entity{} );
        auto prefab = assets.FindByHandle<Assets::PrefabAsset>( handle );
        if ( !prefab || !prefab->IsReadyForUse() )
            return Common::MakeFormattedError<ECS::Entity>(
                 "level '{}': its Player Controller (asset handle {}) is not a loaded prefab",
                 scene.GetSceneName(), static_cast<uint64_t>( handle ) );
        auto placed = prefab->Instantiate( &scene, assets );
        if ( !placed )
            return Common::MakeError<ECS::Entity>(
                 "level '" + scene.GetSceneName() +
                 "': the Player Controller could not be spawned: " + placed.GetError() );
        scene.SetPlayerController( placed.GetValue().GetHandle() );
        return placed;
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

        const auto controller = SpawnPlayerController( scene, assets );
        if ( !controller )
        {
            if ( pawn.GetValue() )
                scene.DestroyEntity( pawn.GetValue() );
            scene.SetPlayerPawn( entt::null );
            return Common::MakeError( controller.GetError() );
        }

        scene.SetPlayFromHere( request.SpawnAt.has_value() );
        if ( const auto view = scene.ResolveViewTarget(); !view )
        {
            if ( pawn.GetValue() )
                scene.DestroyEntity( pawn.GetValue() );
            if ( controller.GetValue() )
                scene.DestroyEntity( controller.GetValue() );
            scene.SetPlayerPawn( entt::null );
            scene.SetPlayerController( entt::null );
            scene.SetPlayFromHere( false );
            return Common::MakeError( view.GetError() );
        }
        // The game rules start with the pawn (UE: the GameMode's RestartPlayer has just run for the player).
        scene.GetGameMode().Begin( request.PlayerStartTag );
        // THE GAME'S UI CLIPS START FROM THEIR FIRST FRAME. A clip's player is runtime state on the component
        // (UIAnimData::Playback) and an authored level may hold one — scrubbed by the Sequencer, or left
        // wherever editing put it. UE constructs fresh widgets for a PIE world; here the players are dropped,
        // and the first game frame re-creates them at Start and honours AutoPlay (UIAnimationPlayback.hpp).
        for ( const auto e : scene.GetRegistry().view<ECS::UIAnimComponent>() )
            scene.GetRegistry().get<ECS::UIAnimComponent>( e ).Data.Playback.reset();
        scene.SetState( Scene::SceneState::Play );
        return BOOLSUCCESS;
    }
} // namespace Desert::Core
