#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>
#include <Engine/ECS/System/System.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

#include <Common/Core/Logger.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::ECS
{
    /**
     * @brief The clip source of a level sequence's Animation sections over @p assets (UE: the section's
     * UAnimSequence reference resolved by the asset system): the section's clip GUID → its AnimationAsset,
     * loaded when not resident. nullptr (the host refuses the section by name) when no such asset exists or it
     * fails to load. Shared by the play-time system and the Sequencer's preview so both play the same clip.
     */
    [[nodiscard]] inline LevelSequenceClipSource LevelSequenceClips( Assets::AssetManager& assets )
    {
        return [&assets]( const Common::Content::AssetGuid& guid ) -> const Animation::AnimationClip*
        {
            if ( guid.IsNull() )
                return nullptr;
            auto asset = assets.ProbeByHandle<Assets::AnimationAsset>(
                 Assets::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ) );
            if ( !asset || !asset->EnsureLoaded( assets ) )
                return nullptr;
            return &asset->GetClip();
        };
    }

    /**
     * @brief The material slots a Material Parameter track writes (UE: MovieSceneComponentMaterialTrack's
     * dynamic instance on the component): slot @p parameter.Slot's runtime material instance of the entity's
     * Static or Skinned mesh — the instance the PBR / slot draws bind, which MeshECSSystem builds per entity from
     * the slot's asset, so the asset itself is never written. A mesh with no authored slot (it draws the shared
     * engine default instance), a slot past the authored ones, or a slot whose instance is not built yet is not
     * the actor's to write: Set is false and the host refuses the track by name. Shared by the play-time system
     * and the Sequencer's preview, like the clip source above.
     */
    [[nodiscard]] inline LevelSequenceMaterialSlots LevelSequenceMaterialSlotOverrides()
    {
        const auto instanceOf = []( entt::registry& registry, const entt::entity entity,
                                    const uint32_t slot ) -> Graphic::MaterialInstance*
        {
            const auto pick = [slot]( const auto& mesh ) -> Graphic::MaterialInstance*
            {
                if ( slot >= mesh.MaterialSlots.size() || slot >= mesh.RuntimeMaterialInstances.size() )
                    return nullptr;
                return mesh.RuntimeMaterialInstances[slot].get();
            };
            if ( !registry.valid( entity ) )
                return nullptr;
            if ( const auto* mesh = registry.try_get<StaticMeshComponent>( entity ) )
                return pick( *mesh );
            if ( const auto* mesh = registry.try_get<SkinnedMeshComponent>( entity ) )
                return pick( *mesh );
            return nullptr;
        };
        LevelSequenceMaterialSlots slots;
        slots.Get = [instanceOf]( entt::registry& registry, const entt::entity entity,
                                  const LevelSequenceMaterialParameter& parameter ) -> std::optional<glm::vec4>
        {
            const auto* instance = instanceOf( registry, entity, parameter.Slot );
            return instance != nullptr ? instance->GetOverrideAsVec4( parameter.Name ) : std::nullopt;
        };
        slots.Set = [instanceOf]( entt::registry& registry, const entt::entity entity,
                                  const LevelSequenceMaterialParameter& parameter,
                                  const std::optional<glm::vec4>&       value ) -> bool
        {
            auto* instance = instanceOf( registry, entity, parameter.Slot );
            if ( instance == nullptr )
                return false;
            if ( value )
                return instance->SetParamFromVec4( parameter.Name, *value );
            instance->ClearOverride( parameter.Name );
            return true;
        };
        return slots;
    }

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
            if ( m_Scene == nullptr || m_AssetManager == nullptr ||
                 m_Scene->GetState() != Core::Scene::SceneState::Play )
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
                const LevelSequenceStep result = StepLevelSequence( registry, component, *actor->Playback, step,
                                                                    LevelSequenceClips( *m_AssetManager ),
                                                                    LevelSequenceMaterialSlotOverrides() );
                for ( const auto& error : TakeNewLevelSequenceErrors( actor->State, result ) )
                    LOG_ERROR( "[LevelSequence] '{}': {}", actor->Name, error );
                for ( const auto& name : result.FiredEvents )
                    LOG_INFO( "[LevelSequence] '{}': event '{}'", actor->Name, name );

                if ( const auto target =
                          LevelSequenceViewTarget( actor->State, result, m_Scene->GetViewTarget() ) )
                    m_Scene->SetViewTarget( *target );
            }

            // An actor whose entity was destroyed in Play (a streamed-out cell, a delete) or lost its component
            // is retired: a cut it held in force gives the view target back, as its end would have, and its row
            // goes with the loaded sequence it pinned.
            for ( auto it = m_Actors.begin(); it != m_Actors.end(); )
            {
                if ( registry.valid( it->first ) && registry.has<LevelSequenceComponent>( it->first ) )
                {
                    ++it;
                    continue;
                }
                if ( it->second.State.CutInForce )
                    m_Scene->SetViewTarget( it->second.State.TargetBeforeCut );
                it = m_Actors.erase( it );
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
            if ( component.Sequence == 0 )
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
