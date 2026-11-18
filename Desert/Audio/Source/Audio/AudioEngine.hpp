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
    // A VOICE — one playing instance of a sound (UE FActiveSound): the slot it lives in and that slot's
    // generation. A slot's generation moves on every release, so a handle kept past its voice (a component
    // that outlived StopAll, a sequence section that already stopped) names nothing instead of a stranger.
    // The default handle is invalid; a live voice's generation is never 0.
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

    // Engine-wide audio playback on miniaudio. Two kinds of voice:
    //   - one-shots: fire-and-forget (Lua `Audio.play`, UI clicks, sequence events) — reclaimed when done;
    //   - managed voices: owned by AudioECSSystem for AudioSourceComponents and sequence Audio sections —
    //     created/positioned/stopped explicitly by handle.
    // LOADING (AUD-CORE): every clip is opened through miniaudio's resource manager, whose VFS is the engine's
    // FileSystem (VFS-aware: loose files in dev, the mounted .dpak in a packaged game). The file read and the
    // decode run on the resource manager's job thread, never on the caller's; a Resident sound's decoded PCM
    // is one buffer shared by all its voices, a Streamed sound decodes page by page (SoundProperties.hpp).
    // All calls are main-thread; mixing runs on the miniaudio device thread, which reads every voice parameter
    // (volume, position, playing) the setters below store atomically.
    class AudioEngine
    {
    public:
        static AudioEngine& Get();

        ~AudioEngine();

        // Lazy device init on first use; returns false when no audio device is available (the engine
        // then no-ops instead of crashing — e.g. CI runners).
        bool EnsureInitialized();

        // 3D listener (the active camera). Safe to call every frame.
        void SetListener( const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up );

        // Fire-and-forget playback (2D, non-spatial), in the source's class and with its loading.
        void PlayOneShot( const SoundSource& source, float volume = 1.0f );

        // Managed voices (AudioECSSystem). An invalid handle when the clip cannot be opened (logged by name)
        // or there is no audio device.
        [[nodiscard]] VoiceHandle CreateVoice( const SoundSource& source, bool loop, bool spatial, float volume );
        void                      DestroyVoice( VoiceHandle voice );
        void                      StartVoice( VoiceHandle voice );
        void                      StopVoice( VoiceHandle voice );
        [[nodiscard]] bool        IsVoicePlaying( VoiceHandle voice ) const;
        // Moves the voice's play position to @p seconds into its clip (a sequence's Audio section following
        // a scrub or a loop, ECS/System/AudioECSSystem.hpp). Clamped at 0; past the end the voice ends. A voice
        // still loading takes the seek on its first read.
        void SeekVoice( VoiceHandle voice, double seconds );
        void SetVoicePosition( VoiceHandle voice, const glm::vec3& position );
        void SetVoiceVolume( VoiceHandle voice, float volume );

        // Stops + destroys every voice, managed and one-shot (Play -> Edit transition). Every handle given out
        // before names nothing afterwards.
        void StopAll();

        // The miniaudio engine itself, for a producer that streams its own PCM into the mix (Media's
        // MediaAudioOutput). Initializes the device on first use; nullptr when there is no audio device.
        ma_engine* GetNativeEngine();
        // The sound group of @p soundClass, for such a producer to play inside its class. nullptr when there is
        // no audio device.
        ma_sound* GetClassGroup( SoundClass soundClass );

        // Per-frame housekeeping: reclaims finished one-shots.
        void Update();

    private:
        AudioEngine();

        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Audio
