#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Audio/AudioEngine.hpp>

#include <glm/glm.hpp>

#include <unordered_map>

namespace Desert::ECS
{
    // Drives AudioSourceComponents (Play only): AutoPlay sources start when the scene enters Play,
    // spatial sources follow their entity's world transform, and the listener follows the active
    // camera. On the Play->Edit transition every source stops — Edit mode is silent, matching how
    // physics/scripts freeze. The system OWNS the runtime source ids (the component stays pure data,
    // so Play never dirties the authored scene).
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT: sound is not a picture. An ambience emitter carries
    // no mesh at all, so hiding one in the outliner to declutter the scene would silently mute the level.
    // Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class AudioECSSystem final : public System
    {
    public:
        explicit AudioECSSystem( Core::Scene* scene ) : m_Scene( scene )
        {
        }

        ~AudioECSSystem() override
        {
            StopEverything();
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ) override
        {
            using SceneState = Core::Scene::SceneState;
            const bool playing = m_Scene && m_Scene->GetState() == SceneState::Play;

            auto& audio = Audio::AudioEngine::Get();

            if ( !playing )
            {
                if ( m_WasPlaying )
                    StopEverything();
                return;
            }
            m_WasPlaying = true;

            // Listener = the active camera (gameplay camera in Play; editor camera as the fallback).
            if ( const auto camera = m_Scene->GetActiveCamera() )
            {
                const glm::mat4 view    = camera->GetViewMatrix();
                const glm::vec3 forward = -glm::vec3( view[0][2], view[1][2], view[2][2] );
                const glm::vec3 up      = glm::vec3( view[0][1], view[1][1], view[2][1] );
                audio.SetListener( camera->GetPosition(), forward, up );
            }

            auto view = registry.view<AudioSourceComponent, TransformComponent>();
            for ( auto entity : view )
            {
                const auto& source    = view.get<AudioSourceComponent>( entity ).Data;
                const auto& transform = view.get<TransformComponent>( entity );

                auto it = m_Sources.find( entity );
                if ( it == m_Sources.end() )
                {
                    if ( !source.AutoPlay || source.Clip.empty() )
                        continue;
                    const uint32_t id =
                         audio.CreateSource( source.Clip, source.Loop, source.Spatial, source.Volume );
                    if ( id == 0 )
                    {
                        m_Sources.emplace( entity, 0 ); // failed clip: don't retry every frame
                        continue;
                    }
                    audio.StartSource( id );
                    it = m_Sources.emplace( entity, id ).first;
                }

                if ( it->second == 0 )
                    continue;

                audio.SetSourceVolume( it->second, source.Volume );
                if ( source.Spatial )
                    audio.SetSourcePosition( it->second, transform.Translation );
            }

            // Entities whose component/entity vanished mid-Play release their source.
            for ( auto it = m_Sources.begin(); it != m_Sources.end(); )
            {
                if ( !registry.valid( it->first ) || !registry.has<AudioSourceComponent>( it->first ) )
                {
                    audio.DestroySource( it->second );
                    it = m_Sources.erase( it );
                }
                else
                    ++it;
            }

            audio.Update(); // reclaim finished one-shots (Lua Audio.play)
        }

    private:
        void StopEverything()
        {
            auto& audio = Audio::AudioEngine::Get();
            for ( const auto& [entity, id] : m_Sources )
                audio.DestroySource( id );
            m_Sources.clear();
            audio.StopAll(); // also drops Lua one-shots
            m_WasPlaying = false;
        }

        Core::Scene*                                m_Scene = nullptr;
        std::unordered_map<entt::entity, uint32_t>  m_Sources;
        bool                                        m_WasPlaying = false;
    };
} // namespace Desert::ECS
