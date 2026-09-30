#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>
#include <Engine/ECS/System/System.hpp>

#include <Common/Core/Logger.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::ECS
{
    /**
     * @brief Plays every LevelSequenceComponent of the scene in Play (UE: ALevelSequenceActor's player,
     * ticking only in PIE/game — the editor scrubs through the Sequencer, not through this system).
     *
     * Per actor: the `.dseq` read through the loader, a Player over its range (Loop from the component,
     * started when AutoPlay), the Evaluator applying each step through LevelSequenceEntityHost. A Camera Cut
     * moves the scene's view target to the cut's camera and gives the previous target back when no cut is
     * in force. Unresolved bindings and refused tracks are LOGGED ONCE each, by label — never silent.
     */
    class LevelSequenceSystem final : public System
    {
    public:
        LevelSequenceSystem( Core::Scene* scene, Assets::AssetManager* assetManager )
             : m_Scene( scene ), m_AssetManager( assetManager )
        {
        }

        void Update( entt::registry&         registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ts ) override
        {
            if ( !m_Scene || !m_AssetManager || m_Scene->GetState() != Core::Scene::SceneState::Play )
            {
                m_Actors.clear(); // Stop restores the pre-Play scene; the next Play starts every actor afresh
                return;
            }

            for ( const auto entity : registry.view<LevelSequenceComponent>() )
            {
                const auto& component = registry.get<LevelSequenceComponent>( entity );
                Actor*      actor     = ActorFor( entity, component );
                if ( actor == nullptr )
                    continue;

                const Animation::Timeline::TimeStep step = actor->Playback->Player.Advance( ts.GetSeconds() );
                const LevelSequenceStep result = StepLevelSequence( registry, component, *actor->Playback, step );
                for ( const auto& error : TakeNewLevelSequenceErrors( actor->State, result ) )
                    LOG_ERROR( "[LevelSequence] '{}': {}", actor->Name, error );
                for ( const auto& name : result.FiredEvents )
                    LOG_INFO( "[LevelSequence] '{}': event '{}'", actor->Name, name );

                if ( const auto target =
                          LevelSequenceViewTarget( actor->State, result, m_Scene->GetViewTarget() ) )
                    m_Scene->SetViewTarget( *target );
            }
        }

    private:
        struct Actor
        {
            Assets::Asset<Assets::LevelSequenceAsset> Asset;
            std::unique_ptr<LevelSequencePlayback>    Playback;
            std::string                               Name;
            LevelSequenceActorState                   State;
        };

        Actor* ActorFor( entt::entity entity, const LevelSequenceComponent& component )
        {
            if ( auto found = m_Actors.find( entity ); found != m_Actors.end() )
                return found->second.Playback ? &found->second : nullptr;

            Actor& actor = m_Actors[entity];
            actor.Name   = m_Scene->GetRegistry().has<TagComponent>( entity )
                                ? m_Scene->GetRegistry().get<TagComponent>( entity ).Tag
                                : std::string( "LevelSequence" );
            if ( !component.Sequence )
            {
                LOG_ERROR( "[LevelSequence] '{}' names no sequence; nothing plays", actor.Name );
                return nullptr;
            }
            actor.Asset = m_AssetManager->FindByHandle<Assets::LevelSequenceAsset>( component.Sequence );
            if ( const auto loaded = Assets::LoadThroughLoader( *m_AssetManager, actor.Asset ); !loaded )
            {
                LOG_ERROR( "[LevelSequence] '{}': sequence handle {} could not be read: {}", actor.Name,
                           static_cast<uint64_t>( component.Sequence ), loaded.GetError() );
                return nullptr;
            }
            actor.Playback = std::make_unique<LevelSequencePlayback>( actor.Asset->GetSequence() );
            actor.Playback->Player.SetLoopMode( component.Loop );
            if ( component.AutoPlay )
                actor.Playback->Player.Play();
            return &actor;
        }

        Core::Scene*                            m_Scene        = nullptr;
        Assets::AssetManager*                   m_AssetManager = nullptr;
        std::unordered_map<entt::entity, Actor> m_Actors;
    };
} // namespace Desert::ECS
