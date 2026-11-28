#pragma once

#include <Common/Core/Core.hpp>
#include <Audio/AudioMix.hpp>
#include <Audio/SoundProperties.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

struct ma_engine;
struct ma_sound; // ma_sound_group is a ma_sound

namespace Desert::Audio
{
    // A VOICE — one playing instance of a sound: the slot it lives in and that slot's generation. The default
    // handle is invalid; a live voice's generation is never 0.
    struct VoiceHandle
    {
        uint32_t Index      = 0;
        uint32_t Generation = 0;

        [[nodiscard]] bool IsValid() const
        {
            return Generation != 0;
        }
        bool operator==( const VoiceHandle& ) const = default;
    };

    // Engine-wide audio playback on miniaudio. One sound group per class of AudioMix::Classes(), created parent
    // first with the parent's group as its output, so a class's volume and pitch reach every descendant through
    // the node graph. On MixChange::Classes every voice is stopped (StopAll) and the groups are rebuilt; on
    // MixChange::Volumes only the group volumes are pushed. All calls are main-thread.
    class AudioEngine
    {
    public:
        static AudioEngine& Get();

        ~AudioEngine();

        // Lazy device init on first use; false when no audio device is available.
        bool EnsureInitialized();

        void SetListener( const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up );

        // Fire-and-forget playback (2D), in the source's class. A class AudioMix does not hold: logged with the
        // clip and the GUID, nothing plays.
        void PlayOneShot( const SoundSource& source, float volume = 1.0f );

        // Managed voices. An invalid handle when the clip cannot be opened, the class is not in AudioMix (both
        // logged by name) or there is no audio device.
        [[nodiscard]] VoiceHandle CreateVoice( const SoundSource& source, bool loop, bool spatial, float volume );
        void                      DestroyVoice( VoiceHandle voice );
        void                      StartVoice( VoiceHandle voice );
        void                      StopVoice( VoiceHandle voice );
        [[nodiscard]] bool        IsVoicePlaying( VoiceHandle voice ) const;
        void                      SeekVoice( VoiceHandle voice, double seconds );
        void                      SetVoicePosition( VoiceHandle voice, const glm::vec3& position );
        void                      SetVoiceVolume( VoiceHandle voice, float volume );

        // Stops + destroys every voice, managed and one-shot.
        void StopAll();

        // The miniaudio engine, for a producer that streams its own PCM into the mix. nullptr without a device.
        ma_engine* GetNativeEngine();
        // The sound group of @p soundClass, for such a producer. nullptr without a device or when AudioMix does
        // not hold the class (logged with the GUID). The pointer dies on the next MixChange::Classes: a producer
        // compares AudioMix::Version() and asks again.
        ma_sound* GetClassGroup( const SoundClassId& soundClass );

        // Per-frame housekeeping: reclaims finished one-shots.
        void Update();

    private:
        AudioEngine();

        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Audio
