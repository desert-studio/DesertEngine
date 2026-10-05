#include "MediaPlayer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace Desert::Media
{
    namespace
    {
        // How far ahead of the clock the sound is decoded: enough to ride out a long frame without the device
        // running dry.
        constexpr uint64_t kAudioLeadFrames = OpusAudioDecoder::kSampleRate * 3 / 10;
        constexpr size_t   kMaxQueuedVideo  = 8; // decoded pictures ahead of the clock, at most
    } // namespace

    MediaPlayer::~MediaPlayer()
    {
        StopDecoding();
    }

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
        if ( m_Sink != nullptr && m_Audio )
            if ( std::string error = m_Sink->Start( OpusAudioDecoder::kSampleRate, m_Audio->Channels() );
                 !error.empty() )
            {
                Fail( std::format( "{}: audio output: {}", source.Path.string(), error ) );
                return m_Error;
            }
        Rewind( 0 );
        // The first frame is there to show before Play: an open is a preroll (UE's media "prepared"), so it
        // waits for that one picture; the frames after it never are.
        WaitForFrameAt( 0 );
        Tick( 0.0 );
        return m_State == MediaPlayerState::Error ? m_Error : std::string{};
    }

    void MediaPlayer::Close()
    {
        StopDecoding();
        if ( m_Sink != nullptr )
            m_Sink->Flush();
        m_Demuxer = WebmDemuxer{};
        m_Video.reset();
        m_Audio.reset();
        m_Queue.clear();
        m_Current.reset();
        m_State      = MediaPlayerState::Closed;
        m_DurationNs = 0;
        m_DemuxDone  = false;
        m_DecodeError.clear();
        m_Error.clear();
    }

    void MediaPlayer::SetAudioSink( IMediaAudioSink* sink )
    {
        const bool open = m_State != MediaPlayerState::Closed && m_State != MediaPlayerState::Error;
        if ( open )
            StopDecoding(); // the thread pushes into the sink being replaced
        const int64_t clock = open ? ClockNs() : 0;
        m_Sink              = sink;
        if ( !open )
            return;
        if ( m_Sink != nullptr && m_Audio )
            if ( std::string error = m_Sink->Start( OpusAudioDecoder::kSampleRate, m_Audio->Channels() );
                 !error.empty() )
            {
                Fail( std::format( "audio output: {}", error ) );
                return;
            }
        Rewind( clock );
    }

    void MediaPlayer::Play()
    {
        if ( m_State != MediaPlayerState::Stopped && m_State != MediaPlayerState::Paused )
            return;
        m_State = MediaPlayerState::Playing;
        if ( m_Sink != nullptr )
            m_Sink->SetPaused( false );
    }

    void MediaPlayer::Pause()
    {
        if ( m_State != MediaPlayerState::Playing )
            return;
        m_State = MediaPlayerState::Paused;
        if ( m_Sink != nullptr )
            m_Sink->SetPaused( true );
    }

    void MediaPlayer::Stop()
    {
        if ( m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return;
        m_State = MediaPlayerState::Stopped;
        Rewind( 0 );
        WaitForFrameAt( 0 );
        Tick( 0.0 );
    }

    bool MediaPlayer::Seek( double seconds )
    {
        if ( m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return false;
        const int64_t target =
             std::clamp<int64_t>( static_cast<int64_t>( std::llround( seconds * 1e9 ) ), 0, m_DurationNs );
        StopDecoding();
        if ( !m_Demuxer.Seek( target ) )
        {
            m_Error = m_Demuxer.Error();
            StartDecoding(); // the position is unchanged; decoding carries on from it
            return false;
        }
        Rewind( target );
        // A seek lands on the frame at the target before it returns (as Open shows the first one).
        WaitForFrameAt( target );
        Tick( 0.0 );
        return true;
    }

    int64_t MediaPlayer::ClockNs() const
    {
        if ( m_Sink != nullptr && m_Audio )
            return m_OriginNs +
                   static_cast<int64_t>( m_Sink->PlayedFrames() * 1000000000ull / OpusAudioDecoder::kSampleRate );
        return m_InternalNs;
    }

    void MediaPlayer::Rewind( int64_t targetNs )
    {
        StopDecoding();
        m_Demuxer.Seek( targetNs ); // to 0 always succeeds; a non-zero target was checked by Seek()
        if ( m_Video )
            m_Video->Flush();
        if ( m_Audio )
            m_Audio->Reset( targetNs == 0 );
        if ( m_Sink != nullptr )
        {
            m_Sink->Flush();
            m_Sink->SetPaused( m_State != MediaPlayerState::Playing );
        }
        m_Queue.clear();
        m_OriginNs          = targetNs;
        m_InternalNs        = targetNs;
        m_DropAudioBeforeNs = targetNs;
        m_DemuxDone         = false;
        m_DecodeError.clear();
        m_DecodeClockNs.store( targetNs );
        StartDecoding();
    }

    void MediaPlayer::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_State = MediaPlayerState::Error;
    }

    void MediaPlayer::StartDecoding()
    {
        if ( m_Decoder.joinable() || m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return;
        m_StopDecoding = false;
        m_Decoder      = std::thread( [this] { DecodeLoop(); } );
    }

    void MediaPlayer::StopDecoding()
    {
        if ( !m_Decoder.joinable() )
            return;
        {
            const std::lock_guard lock( m_Lock );
            m_StopDecoding = true;
        }
        m_Changed.notify_all();
        m_Decoder.join();
    }

    void MediaPlayer::DecodeLoop()
    {
        // Sound is decoded ahead in every state: a paused sink buffers it, so Play starts with it queued.
        const bool wantSound = m_Sink != nullptr && m_Audio;
        for ( ;; )
        {
            {
                // Read under the lock the owner publishes it under: a clock moved between the read and the wait
                // below would otherwise be a lost wake-up.
                std::unique_lock lock( m_Lock );
                const int64_t    clockNs = wantSound ? ClockNs() : m_DecodeClockNs.load();
                if ( m_StopDecoding || m_DemuxDone || !m_DecodeError.empty() )
                    return;
                // Frames the clock has already passed are dropped as they arrive (the skip Present does), so a
                // decoder behind the clock catches up instead of filling the queue with the past.
                while ( m_Queue.size() >= 2 && m_Queue[1].PtsNs <= clockNs )
                    m_Queue.pop_front();
                const bool needVideo = m_Video && m_Queue.size() < kMaxQueuedVideo;
                const bool needAudio = wantSound && m_Sink->QueuedFrames() < kAudioLeadFrames;
                if ( m_Queue.size() >= kMaxQueuedVideo || ( !needVideo && !needAudio ) )
                {
                    // Full, or nothing wanted: sleep until the owner takes a frame or the clock moves. The
                    // sound drains on the device's thread without telling anyone, so with a sink the wait is
                    // bounded (well under the sound lead); without one every change is a notification.
                    if ( wantSound )
                        m_Changed.wait_for( lock, std::chrono::milliseconds( 4 ) );
                    else
                        m_Changed.wait( lock );
                    continue;
                }
            }
            if ( !DecodeOne() )
            {
                m_Changed.notify_all();
                return;
            }
        }
    }

    bool MediaPlayer::DecodeOne()
    {
        MediaPacket packet;
        if ( !m_Demuxer.NextPacket( packet ) )
        {
            std::string error = m_Demuxer.Error();
            m_Decoded.clear();
            if ( error.empty() && m_Video )
                m_Video->Drain( m_Decoded );
            const std::lock_guard lock( m_Lock );
            for ( VideoFrame& f : m_Decoded )
                m_Queue.push_back( std::move( f ) );
            m_DecodeError = std::move( error );
            m_DemuxDone   = true;
            return false;
        }

        if ( packet.Kind == MediaTrackKind::Video && m_Video )
        {
            m_Decoded.clear();
            if ( !m_Video->Decode( packet, m_Decoded ) )
            {
                const std::lock_guard lock( m_Lock );
                m_DecodeError = m_Video->Error();
                return false;
            }
            if ( !m_Decoded.empty() )
            {
                {
                    const std::lock_guard lock( m_Lock );
                    for ( VideoFrame& f : m_Decoded )
                        m_Queue.push_back( std::move( f ) );
                }
                m_Changed.notify_all();
            }
        }
        else if ( packet.Kind == MediaTrackKind::Audio && m_Audio && m_Sink != nullptr )
        {
            m_Pcm.clear();
            const int64_t frames = m_Audio->Decode( packet, m_Pcm );
            if ( frames < 0 )
            {
                const std::lock_guard lock( m_Lock );
                m_DecodeError = m_Audio->Error();
                return false;
            }
            // After a seek the restart cluster begins before the target: drop the sound before it, so
            // played-frame 0 is exactly the target time.
            int64_t skip = 0;
            if ( packet.PtsNs < m_DropAudioBeforeNs && m_DropAudioBeforeNs > 0 )
                skip = std::min<int64_t>( frames, ( m_DropAudioBeforeNs - packet.PtsNs ) *
                                                       OpusAudioDecoder::kSampleRate / 1000000000 );
            if ( frames > skip )
                m_Sink->Push( m_Pcm.data() + skip * m_Audio->Channels(), static_cast<uint64_t>( frames - skip ) );
        }
        return true;
    }

    void MediaPlayer::WaitForFrameAt( int64_t clockNs )
    {
        // Until the queue holds a frame AFTER the clock (so the one due is known), or there will be no more.
        std::unique_lock lock( m_Lock );
        m_Changed.wait( lock,
                        [&]
                        {
                            return !m_Video || m_DemuxDone || !m_DecodeError.empty() || !m_Decoder.joinable() ||
                                   ( !m_Queue.empty() && m_Queue.back().PtsNs > clockNs ) ||
                                   m_Queue.size() >= kMaxQueuedVideo;
                        } );
    }

    void MediaPlayer::Present( int64_t clockNs )
    {
        // Frame SKIP: every queued frame whose successor is also due is dropped unseen. Frame REPEAT: when
        // the next frame is not due yet the current one simply stays.
        bool taken = false;
        {
            const std::lock_guard lock( m_Lock );
            while ( m_Queue.size() >= 2 && m_Queue[1].PtsNs <= clockNs )
                m_Queue.pop_front();
            if ( !m_Queue.empty() && ( m_Queue.front().PtsNs <= clockNs || !m_Current ) )
            {
                m_Current = std::move( m_Queue.front() );
                m_Queue.pop_front();
                taken = true;
            }
        }
        if ( taken )
        {
            ++m_FrameSerial;
            m_Changed.notify_all(); // room in the queue
        }
    }

    void MediaPlayer::Tick( double deltaSeconds )
    {
        if ( m_State == MediaPlayerState::Closed || m_State == MediaPlayerState::Error )
            return;
        if ( m_State == MediaPlayerState::Playing && !( m_Sink != nullptr && m_Audio ) )
            m_InternalNs += static_cast<int64_t>( std::llround( deltaSeconds * 1e9 ) );

        const int64_t clock = ClockNs();
        {
            const std::lock_guard lock( m_Lock );
            m_DecodeClockNs.store( clock );
        }
        m_Changed.notify_all();
        if ( m_BlockOnTime )
            WaitForFrameAt( clock );

        std::string decodeError;
        {
            const std::lock_guard lock( m_Lock );
            decodeError = m_DecodeError;
        }
        if ( !decodeError.empty() )
        {
            StopDecoding();
            Fail( std::move( decodeError ) );
            return;
        }
        Present( clock );

        bool drained = false;
        {
            const std::lock_guard lock( m_Lock );
            drained = m_DemuxDone && m_Queue.empty();
        }
        if ( m_State != MediaPlayerState::Playing || !drained )
            return;
        // The end: every frame shown and, with a sink, every sample played; without one, the clock has
        // passed the container's duration.
        const bool soundDone =
             ( m_Sink != nullptr && m_Audio ) ? m_Sink->QueuedFrames() == 0 : clock >= m_DurationNs;
        if ( !soundDone )
            return;
        if ( m_Looping )
        {
            Rewind( 0 );
            if ( OnEndReached )
                OnEndReached();
            if ( m_BlockOnTime )
                WaitForFrameAt( 0 );
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
