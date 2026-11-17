#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Core/Scene.hpp>
#include <Audio/AudioEngine.hpp>
#include <Engine/Assets/SoundAsset.hpp>

#include <glm/glm.hpp>

#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    // Drives AudioSourceComponents (Play only): AutoPlay sources start when the scene enters Play,
    // spatial sources follow their entity's WORLD transform (a child emitter sounds where it is drawn, UE
    // USceneComponent::GetComponentLocation), every voice plays in the class and with the loading its
    // `.desound` states (SoundAsset::Resolve), and the listener follows the active
    // camera. On the Play->Edit transition every source stops — Edit mode is silent, matching how
    // physics/scripts freeze. The system OWNS the runtime voice handles (the component stays pure data,
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
                    if ( !source.AutoPlay || static_cast<uint64_t>( source.Sound ) == 0 )
                        continue;
                    const auto sound = Assets::SoundAsset::Resolve( source.Sound );
                    if ( !sound )
                    {
                        LOG_ERROR( "[Audio] Audio Source: {}", sound.GetError() );
                        m_Sources.emplace( entity, Audio::VoiceHandle{} ); // one line, no retry every frame
                        continue;
                    }
                    const Audio::VoiceHandle voice =
                         audio.CreateVoice( sound.GetValue(), source.Loop, source.Spatial, source.Volume );
                    if ( !voice.IsValid() )
                    {
                        m_Sources.emplace( entity, voice ); // failed clip: don't retry every frame
                        continue;
                    }
                    audio.StartVoice( voice );
                    it = m_Sources.emplace( entity, voice ).first;
                }

                if ( !it->second.IsValid() )
                    continue;

                audio.SetVoiceVolume( it->second, source.Volume );
                if ( source.Spatial )
                    audio.SetVoicePosition( it->second, glm::vec3( transform.GetWorldTransform()[3] ) );
            }

            PlaySequenceAudio( registry, audio );

            // Entities whose component/entity vanished mid-Play release their source.
            for ( auto it = m_Sources.begin(); it != m_Sources.end(); )
            {
                if ( !registry.valid( it->first ) || !registry.has<AudioSourceComponent>( it->first ) )
                {
                    audio.DestroyVoice( it->second );
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
                    voice.Owner =
                         static_cast<uint64_t>( static_cast<std::underlying_type_t<entt::entity>>( entity ) );
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
                        // The section names the sound's GUID; its file is the `.desound`'s to say (SoundAsset).
                        const auto sound = Assets::SoundAsset::Resolve( command.Sound );
                        if ( !sound )
                        {
                            LOG_ERROR( "[Audio] Sequence sound: {}", sound.GetError() );
                            break; // the diff still holds the voice: one line, no retry spam
                        }
                        // In the sound's own class and loading, as the AudioSource path above.
                        const Audio::VoiceHandle voice = audio.CreateVoice(
                             sound.GetValue(), false, false, command.Gain ); // 2D, as UE's master track
                        if ( !voice.IsValid() )
                            break; // CreateVoice logged the path; the diff still holds the voice, no retry spam
                        audio.SeekVoice( voice, command.Seconds );
                        audio.StartVoice( voice );
                        m_SequenceSources[key] = voice;
                        break;
                    }
                    case TL::AudioCommandKind::Seek:
                        if ( const auto it = m_SequenceSources.find( key ); it != m_SequenceSources.end() )
                            audio.SeekVoice( it->second, command.Seconds );
                        break;
                    case TL::AudioCommandKind::SetGain:
                        if ( const auto it = m_SequenceSources.find( key ); it != m_SequenceSources.end() )
                            audio.SetVoiceVolume( it->second, command.Gain );
                        break;
                    case TL::AudioCommandKind::Stop:
                        if ( const auto it = m_SequenceSources.find( key ); it != m_SequenceSources.end() )
                        {
                            audio.DestroyVoice( it->second );
                            m_SequenceSources.erase( it );
                        }
                        break;
                }
            }
        }

        void StopEverything()
        {
            auto& audio = Audio::AudioEngine::Get();
            for ( const auto& [key, voice] : m_SequenceSources )
                audio.DestroyVoice( voice );
            m_SequenceSources.clear();
            m_SequenceCommands.clear();
            m_SequenceVoices.StopAll( m_SequenceCommands ); // the sources are gone already; forget the voices
            m_SequenceCommands.clear();
            for ( const auto& [entity, voice] : m_Sources )
                audio.DestroyVoice( voice );
            m_Sources.clear();
            audio.StopAll(); // also drops Lua one-shots
            m_WasPlaying = false;
        }

        Core::Scene*                                m_Scene = nullptr;
        std::unordered_map<entt::entity, Audio::VoiceHandle> m_Sources; // invalid = a clip that failed once
        bool                                        m_WasPlaying = false;

        Animation::Timeline::AudioVoices                             m_SequenceVoices;
        std::vector<Animation::Timeline::SoundingVoice>              m_SequenceFrame;    // reused per frame
        std::vector<Animation::Timeline::AudioCommand>               m_SequenceCommands; // reused per frame
        std::map<std::tuple<uint64_t, uint32_t, uint32_t>, Audio::VoiceHandle>
             m_SequenceSources; // section → voice
    };
} // namespace Desert::ECS
