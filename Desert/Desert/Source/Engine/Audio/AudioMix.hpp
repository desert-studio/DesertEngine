#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace Desert::Audio
{
    // UE's USoundClass, the three a game's options menu shows: every sound plays in exactly one class, and a
    // class's volume scales all of them. Music — score and movies; Effects — the world and the interface;
    // Voice — dialogue.
    enum class SoundClass : uint8_t
    {
        Music,
        Effects,
        Voice,
    };
    inline constexpr std::size_t kSoundClassCount = 3;

    // THE MIX THE PLAYER ASKED FOR — master and per-class volume (UE's SoundMix with class adjusters). One owner
    // of the numbers; GameUserSettings::Apply writes them, AudioEngine hears every change through its one
    // listener and pushes it into miniaudio (the engine volume and one sound group per class). Kept apart from
    // the engine so the volumes exist, and are what the next device gets, even while there is no audio device
    // at all (CI, a machine with none plugged in).
    class AudioMix
    {
    public:
        static AudioMix& Get();

        // 0 = silent, 1 = as authored.
        void                SetMasterVolume( float volume );
        [[nodiscard]] float MasterVolume() const
        {
            return m_Master;
        }

        void                SetClassVolume( SoundClass soundClass, float volume );
        [[nodiscard]] float ClassVolume( SoundClass soundClass ) const
        {
            return m_Classes[static_cast<std::size_t>( soundClass )];
        }

        // The one listener: AudioEngine, once its device exists. Called after every change.
        void SetListener( std::function<void()> listener );

    private:
        void Changed() const;

        float                                  m_Master = 1.0f;
        std::array<float, kSoundClassCount>    m_Classes{ 1.0f, 1.0f, 1.0f };
        std::function<void()>                  m_Listener;
    };
} // namespace Desert::Audio
