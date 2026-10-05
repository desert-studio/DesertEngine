#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Audio/AudioEngine.hpp>

#include <glm/glm.hpp>

#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

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

            PlaySequenceAudio( registry, audio );

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
        // UI clips' Audio tracks (UE's Sequencer Audio track): each UIAnimComponent says which of its
        // sections sound at its playhead (UIAnimData::Sounding, written by the view that drives the clips);
        // the voice diff turns that into Start/Seek/SetGain/Stop (Timeline/AudioVoices.hpp) and this turns
        // those into sources. A clip whose entity is gone says nothing, so its voices stop.
        void PlaySequenceAudio( entt::registry& registry, Audio::AudioEngine& audio )
        {
            namespace TL = Animation::Timeline;
            m_SequenceFrame.clear();
            for ( const auto entity : registry.view<UIAnimComponent>() )
            {
                auto& sounding = registry.get<UIAnimComponent>( entity ).Data.Sounding;
                for ( TL::SoundingVoice& voice : sounding )
                {
                    voice.Owner = static_cast<uint64_t>( static_cast<std::underlying_type_t<entt::entity>>( entity ) );
                    m_SequenceFrame.push_back( std::move( voice ) );
                }
                sounding.clear(); // consumed: a frame the view does not refresh is silence, not a repeat
            }
            m_SequenceCommands.clear();
            m_SequenceVoices.Update( m_SequenceFrame, m_SequenceCommands );
            for ( const TL::AudioCommand& command : m_SequenceCommands )
            {
                const auto key = std::make_tuple( command.Owner, command.Track, command.Section );
                switch ( command.Kind )
                {
                    case TL::AudioCommandKind::Start:
                    {
                        const uint32_t id =
                             audio.CreateSource( command.Sound, false, false, command.Gain ); // 2D, as UE's master track
                        if ( id == 0 )
                            break; // CreateSource logged the path; the diff still holds the voice, no retry spam
                        audio.SeekSource( id, command.Seconds );
                        audio.StartSource( id );
                        m_SequenceSources[key] = id;
                        break;
                    }
                    case TL::AudioCommandKind::Seek:
                        if ( const auto it = m_SequenceSources.find( key ); it != m_SequenceSources.end() )
                            audio.SeekSource( it->second, command.Seconds );
                        break;
                    case TL::AudioCommandKind::SetGain:
                        if ( const auto it = m_SequenceSources.find( key ); it != m_SequenceSources.end() )
                            audio.SetSourceVolume( it->second, command.Gain );
                        break;
                    case TL::AudioCommandKind::Stop:
                        if ( const auto it = m_SequenceSources.find( key ); it != m_SequenceSources.end() )
                        {
                            audio.DestroySource( it->second );
                            m_SequenceSources.erase( it );
                        }
                        break;
                }
            }
        }

        void StopEverything()
        {
            auto& audio = Audio::AudioEngine::Get();
            for ( const auto& [key, id] : m_SequenceSources )
                audio.DestroySource( id );
            m_SequenceSources.clear();
            m_SequenceCommands.clear();
            m_SequenceVoices.StopAll( m_SequenceCommands ); // the sources are gone already; forget the voices
            m_SequenceCommands.clear();
            for ( const auto& [entity, id] : m_Sources )
                audio.DestroySource( id );
            m_Sources.clear();
            audio.StopAll(); // also drops Lua one-shots
            m_WasPlaying = false;
        }

        Core::Scene*                                m_Scene = nullptr;
        std::unordered_map<entt::entity, uint32_t>  m_Sources;
        bool                                        m_WasPlaying = false;

        Animation::Timeline::AudioVoices                              m_SequenceVoices;
        std::vector<Animation::Timeline::SoundingVoice>               m_SequenceFrame;    // reused per frame
        std::vector<Animation::Timeline::AudioCommand>                m_SequenceCommands; // reused per frame
        std::map<std::tuple<uint64_t, uint32_t, uint32_t>, uint32_t> m_SequenceSources;  // voice → source id
    };
} // namespace Desert::ECS
