#include "MediaPlayer.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace Desert::Media
{
    namespace
    {
        // How far ahead of the clock the sound is decoded: enough to ride out a long frame on the main
        // thread without the device running dry.
        constexpr uint64_t kAudioLeadFrames = OpusAudioDecoder::kSampleRate * 3 / 10;
    } // namespace

    std::string MediaPlayer::Open( const MediaSource& source )
    {
        Close();
        if ( std::string error = m_Demuxer.Open( source.Path ); !error.empty() )
        {
            Fail( error );
            return error;
        }
        m_DurationNs = m_Demuxer.DurationNs();
        if ( m_DurationNs <= 0 )
        {
            Fail( std::format( "{}: the Segment Info carries no Duration", source.Path.string() ) );
            return m_Error;
        }
        if ( m_Demuxer.Video().Present )
        {
            m_Video = std::make_unique<Av1VideoDecoder>();
            if ( std::string error = m_Video->Open(); !error.empty() )
            {
                Fail( std::format( "{}: {}", source.Path.string(), error ) );
                return m_Error;
            }
        }
        if ( m_Demuxer.Audio().Present )
        {
            m_Audio = std::make_unique<OpusAudioDecoder>();
            if ( std::string error = m_Audio->Open( m_Demuxer.Audio() ); !error.empty() )
            {
                Fail( std::format( "{}: {}", source.Path.string(), error ) );
                return m_Error;
            }
        }
        m_State = MediaPlayerState::Stopped;
        if ( m_Sink && m_Audio )
            if ( std::string error = m_Sink->Start( OpusAudioDecoder::kSampleRate, m_Audio->Channels() );
                 !error.empty() )
            {
                Fail( std::format( "{}: audio output: {}", source.Path.string(), error ) );
                return m_Error;
            }
        Rewind( 0 );
        Tick( 0.0 ); // the first frame is there to show before Play
        return m_State == MediaPlayerState::Error ? m_Error : std::string{};
    }

    void MediaPlayer::Close()
    {
        if ( m_Sink )
            m_Sink->Flush();
        m_Demuxer = WebmDemuxer{};
        m_Video.reset();
        m_Audio.reset();
        m_Queue.clear();
        m_Current.reset();
        m_State      = MediaPlayerState::Closed;
        m_DurationNs = 0;
        m_Error.clear();
    }

    void MediaPlayer::SetAudioSink( IMediaAudioSink* sink )
    {
        m_Sink = sink;
        if ( m_Sink && m_Audio && m_State != MediaPlayerState::Closed && m_State != MediaPlayerState::Error )
        {
            if ( std::string error = m_Sink->Start( OpusAudioDecoder::kSampleRate, m_Audio->Channels() );
                 !error.empty() )
                return Fail( std::format( "audio output: {}", error ) );
            Rewind( ClockNs() );
        }
    }

    void MediaPlayer::Play()
    {
        if ( m_State != MediaPlayerState::Stopped && m_State != MediaPlayerState::Paused )
            return;
        m_State = MediaPlayerState::Playing;
        if ( m_Sink )
            m_Sink->SetPaused( false );
    }

    void MediaPlayer::Pause()
    {
        if ( m_State != MediaPlayerState::Playing )
            return;
        m_State = MediaPlayerState::Paused;
        if ( m_Sink )
            m_Sink->SetPaused( true );
    }

    void MediaPlayer::Stop()
    {
        if ( m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return;
        m_State = MediaPlayerState::Stopped;
        Rewind( 0 );
        Tick( 0.0 );
    }

    bool MediaPlayer::Seek( double seconds )
    {
        if ( m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return false;
        const int64_t target =
             std::clamp<int64_t>( static_cast<int64_t>( std::llround( seconds * 1e9 ) ), 0, m_DurationNs );
        if ( !m_Demuxer.Seek( target ) )
        {
            m_Error = m_Demuxer.Error();
            return false;
        }
        Rewind( target );
        Tick( 0.0 );
        return true;
    }

    int64_t MediaPlayer::ClockNs() const
    {
        if ( m_Sink && m_Audio )
            return m_OriginNs +
                   static_cast<int64_t>( m_Sink->PlayedFrames() * 1000000000ull / OpusAudioDecoder::kSampleRate );
        return m_InternalNs;
    }

    void MediaPlayer::Rewind( int64_t targetNs )
    {
        m_Demuxer.Seek( targetNs ); // to 0 always succeeds; a non-zero target was checked by Seek()
        if ( m_Video )
            m_Video->Flush();
        if ( m_Audio )
            m_Audio->Reset( targetNs == 0 );
        if ( m_Sink )
        {
            m_Sink->Flush();
            m_Sink->SetPaused( m_State != MediaPlayerState::Playing );
        }
        m_Queue.clear();
        m_OriginNs          = targetNs;
        m_InternalNs        = targetNs;
        m_DropAudioBeforeNs = targetNs;
        m_DemuxDone         = false;
    }

    void MediaPlayer::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_State = MediaPlayerState::Error;
    }

    bool MediaPlayer::Pump( int64_t clockNs )
    {
        // Sound is decoded ahead in every state: a paused sink buffers it, so Play starts with it queued.
        const bool wantSound = m_Sink && m_Audio;
        for ( ;; )
        {
            const bool needVideo = m_Video && ( m_Queue.empty() || m_Queue.back().PtsNs <= clockNs );
            const bool needAudio = wantSound && m_Sink->QueuedFrames() < kAudioLeadFrames;
            if ( m_DemuxDone || ( !needVideo && !needAudio ) )
                return true;

            MediaPacket packet;
            if ( !m_Demuxer.NextPacket( packet ) )
            {
                if ( !m_Demuxer.Error().empty() )
                {
                    Fail( m_Demuxer.Error() );
                    return false;
                }
                m_DemuxDone = true;
                if ( m_Video )
                {
                    m_Decoded.clear();
                    m_Video->Drain( m_Decoded );
                    for ( VideoFrame& f : m_Decoded )
                        m_Queue.push_back( std::move( f ) );
                }
                return true;
            }

            if ( packet.Kind == MediaTrackKind::Video && m_Video )
            {
                m_Decoded.clear();
                if ( !m_Video->Decode( packet, m_Decoded ) )
                {
                    Fail( m_Video->Error() );
                    return false;
                }
                for ( VideoFrame& f : m_Decoded )
                    m_Queue.push_back( std::move( f ) );
            }
            else if ( packet.Kind == MediaTrackKind::Audio && m_Audio && m_Sink )
            {
                m_Pcm.clear();
                const int64_t frames = m_Audio->Decode( packet, m_Pcm );
                if ( frames < 0 )
                {
                    Fail( m_Audio->Error() );
                    return false;
                }
                // After a seek the restart cluster begins before the target: drop the sound before it, so
                // played-frame 0 is exactly the target time.
                int64_t skip = 0;
                if ( packet.PtsNs < m_DropAudioBeforeNs && m_DropAudioBeforeNs > 0 )
                    skip = std::min<int64_t>( frames, ( m_DropAudioBeforeNs - packet.PtsNs ) *
                                                           OpusAudioDecoder::kSampleRate / 1000000000 );
                if ( frames > skip )
                    m_Sink->Push( m_Pcm.data() + skip * m_Audio->Channels(),
                                  static_cast<uint64_t>( frames - skip ) );
            }
        }
    }

    void MediaPlayer::Present( int64_t clockNs )
    {
        // Frame SKIP: every queued frame whose successor is also due is dropped unseen. Frame REPEAT: when
        // the next frame is not due yet the current one simply stays.
        while ( m_Queue.size() >= 2 && m_Queue[1].PtsNs <= clockNs )
            m_Queue.pop_front();
        if ( !m_Queue.empty() && ( m_Queue.front().PtsNs <= clockNs || !m_Current ) )
        {
            m_Current = std::move( m_Queue.front() );
            m_Queue.pop_front();
            ++m_FrameSerial;
        }
    }

    void MediaPlayer::Tick( double deltaSeconds )
    {
        if ( m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return;
        if ( m_State == MediaPlayerState::Playing && !( m_Sink && m_Audio ) )
            m_InternalNs += static_cast<int64_t>( std::llround( deltaSeconds * 1e9 ) );

        int64_t clock = ClockNs();
        if ( !Pump( clock ) )
            return;
        Present( clock );

        if ( m_State != MediaPlayerState::Playing || !m_DemuxDone || !m_Queue.empty() )
            return;
        // The end: every frame shown and, with a sink, every sample played; without one, the clock has
        // passed the container's duration.
        const bool soundDone = m_Sink && m_Audio ? m_Sink->QueuedFrames() == 0 : clock >= m_DurationNs;
        if ( !soundDone )
            return;
        if ( m_Looping )
        {
            Rewind( 0 );
            if ( OnEndReached )
                OnEndReached();
            if ( Pump( 0 ) )
                Present( 0 );
            return;
        }
        // Stopped and rewound, as Stop() leaves it: the next Play() plays the clip again from the start.
        m_State = MediaPlayerState::Stopped;
        Rewind( 0 );
        if ( OnEndReached )
            OnEndReached();
    }
} // namespace Desert::Media
