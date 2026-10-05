#include "AudioVoices.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Animation::Timeline
{
    const char* ToString( const AudioCommandKind kind )
    {
        switch ( kind )
        {
            case AudioCommandKind::Start:
                return "Start";
            case AudioCommandKind::Seek:
                return "Seek";
            case AudioCommandKind::SetGain:
                return "SetGain";
            case AudioCommandKind::Stop:
                return "Stop";
        }
        return "Unknown";
    }

    namespace
    {
        bool SameVoice( const SoundingVoice& a, const SoundingVoice& b )
        {
            return a.Owner == b.Owner && a.Track == b.Track && a.Section == b.Section;
        }

        AudioCommand CommandFor( const AudioCommandKind kind, const SoundingVoice& voice )
        {
            return AudioCommand{ kind, voice.Owner, voice.Track, voice.Section, voice.Sound, voice.Seconds,
                                 voice.Gain };
        }
    } // namespace

    void AudioVoices::Update( const std::vector<SoundingVoice>& frame, std::vector<AudioCommand>& out )
    {
        // Stops first, so an executor that reuses a voice's slot never sees a Start before its Stop.
        for ( const SoundingVoice& playing : m_Voices )
        {
            const auto now = std::ranges::find_if( frame, [&]( const SoundingVoice& v )
                                                   { return SameVoice( v, playing ); } );
            if ( now == frame.end() || now->Sound != playing.Sound )
            {
                out.push_back( CommandFor( AudioCommandKind::Stop, playing ) );
            }
        }

        std::vector<SoundingVoice> next;
        next.reserve( frame.size() );
        for ( const SoundingVoice& voice : frame )
        {
            const auto was = std::ranges::find_if( m_Voices, [&]( const SoundingVoice& v )
                                                   { return SameVoice( v, voice ); } );
            if ( was == m_Voices.end() || was->Sound != voice.Sound )
            {
                out.push_back( CommandFor( AudioCommandKind::Start, voice ) );
            }
            else
            {
                if ( std::abs( voice.Seconds - ( was->Seconds + voice.Advanced ) ) > kSeekTolerance )
                {
                    out.push_back( CommandFor( AudioCommandKind::Seek, voice ) );
                }
                if ( voice.Gain != was->Gain )
                {
                    out.push_back( CommandFor( AudioCommandKind::SetGain, voice ) );
                }
            }
            next.push_back( voice );
        }
        m_Voices = std::move( next );
    }

    void AudioVoices::StopAll( std::vector<AudioCommand>& out )
    {
        for ( const SoundingVoice& playing : m_Voices )
        {
            out.push_back( CommandFor( AudioCommandKind::Stop, playing ) );
        }
        m_Voices.clear();
    }
} // namespace Desert::Animation::Timeline
