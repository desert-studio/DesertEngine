#include "AudioMix.hpp"

#include <utility>

namespace Desert::Audio
{
    AudioMix& AudioMix::Get()
    {
        static AudioMix s_Mix;
        return s_Mix;
    }

    void AudioMix::SetMasterVolume( float volume )
    {
        m_Master = volume;
        Changed();
    }

    void AudioMix::SetClassVolume( SoundClass soundClass, float volume )
    {
        m_Classes[static_cast<std::size_t>( soundClass )] = volume;
        Changed();
    }

    void AudioMix::SetListener( std::function<void()> listener )
    {
        m_Listener = std::move( listener );
    }

    void AudioMix::Changed() const
    {
        if ( m_Listener )
            m_Listener();
    }
} // namespace Desert::Audio
