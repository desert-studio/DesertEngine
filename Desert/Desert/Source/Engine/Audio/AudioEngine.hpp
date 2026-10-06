#pragma once

#include <Common/Core/Core.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct ma_engine;

namespace Desert::Audio
{
    // Engine-wide audio playback on miniaudio. Two kinds of playback:
    //   - one-shots: fire-and-forget (Lua `Audio.play`, UI clicks) — garbage-collected when done;
    //   - managed sources: owned by AudioECSSystem for AudioSourceComponents — created/positioned/
    //     stopped explicitly by id.
    // Clip bytes are read through FileSystem (VFS-aware), so audio plays from loose files in dev and
    // from the mounted .dpak in a packaged game. All calls are main-thread; mixing runs on the
    // miniaudio device thread internally.
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

        // Fire-and-forget playback (2D, non-spatial). Path resolution: as-given, then Assets-relative.
        void PlayOneShot( const std::string& clipPath, float volume = 1.0f );

        // Managed sources (AudioECSSystem). 0 is the invalid id.
        uint32_t CreateSource( const std::string& clipPath, bool loop, bool spatial, float volume );
        void     DestroySource( uint32_t id );
        void     StartSource( uint32_t id );
        void     StopSource( uint32_t id );
        bool     IsSourcePlaying( uint32_t id ) const;
        void     SetSourcePosition( uint32_t id, const glm::vec3& position );
        void     SetSourceVolume( uint32_t id, float volume );

        // Stops + destroys every source and finished one-shots (Play -> Edit transition).
        void StopAll();

        // The miniaudio engine itself, for a producer that streams its own PCM into the mix (Media's
        // MediaAudioOutput). Initializes the device on first use; nullptr when there is no audio device.
        ma_engine* GetNativeEngine();

        // Per-frame housekeeping: reclaims finished one-shots.
        void Update();

    private:
        AudioEngine();

        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Audio
