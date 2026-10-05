#pragma once

#include <Engine/Media/MediaCodecs.hpp>
#include <Engine/Media/WebmDemuxer.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Media
{
    // UE's Media Framework, the same three roles: MediaSource (WHAT to play) → MediaPlayer (the transport:
    // open, play, pause, seek, loop, the clock, OnEndReached) → MediaTexture (WHERE the picture goes, a
    // stable GPU image the UI or a material samples). Sound leaves the player through IMediaAudioSink.

    // A clip: one WebM file (AV1 video and/or Opus audio).
    struct MediaSource
    {
        std::filesystem::path Path;
    };

    // Where decoded sound goes. The sink's PLAYED-frame count is the master clock whenever a sink is
    // attached: the picture follows the sound (frames are skipped or held), never the other way round.
    class IMediaAudioSink
    {
    public:
        virtual ~IMediaAudioSink() = default;

        virtual std::string Start( uint32_t sampleRate, uint32_t channels )   = 0; // empty on success
        virtual void        Push( const float* interleaved, uint64_t frames ) = 0;
        virtual uint64_t    PlayedFrames() const                              = 0; // since Start / Flush
        virtual uint64_t    QueuedFrames() const                              = 0; // pushed, not yet played
        virtual void        SetPaused( bool paused )                          = 0;
        virtual void        Flush() = 0; // drop queued; PlayedFrames → 0
    };

    enum class MediaPlayerState : uint8_t
    {
        Closed,
        Stopped, // opened, or played to the end without looping, or Stop()ped
        Playing,
        Paused,
        Error,
    };

    class MediaPlayer
    {
    public:
        // Fires each time playback reaches the end of the clip — once per pass: with looping on, once per
        // loop (and playback carries on from the start); with it off, once (and the player stops).
        std::function<void()> OnEndReached;

        std::string Open( const MediaSource& source ); // empty on success; Stopped at 0 with the first frame
        void        Close();

        void Play();
        void Pause();
        void Stop(); // Stopped, rewound to the start
        bool Seek( double seconds );
        void SetLooping( bool looping )
        {
            m_Looping = looping;
        }
        bool IsLooping() const
        {
            return m_Looping;
        }

        // Attach before Play. nullptr: the clock is advanced by Tick's delta instead (no sound).
        void SetAudioSink( IMediaAudioSink* sink );

        // Once per frame: advances the clock (when Playing), decodes what the clock needs and selects the
        // frame to show. The current frame changes ⇔ FrameSerial() changes.
        void Tick( double deltaSeconds );

        MediaPlayerState GetState() const
        {
            return m_State;
        }
        double GetDuration() const
        {
            return static_cast<double>( m_DurationNs ) * 1e-9;
        }
        double GetTime() const
        {
            return static_cast<double>( ClockNs() ) * 1e-9;
        }
        bool HasVideo() const
        {
            return m_Demuxer.Video().Present;
        }
        bool HasAudio() const
        {
            return m_Demuxer.Audio().Present;
        }
        uint32_t GetVideoWidth() const
        {
            return m_Demuxer.Video().Width;
        }
        uint32_t GetVideoHeight() const
        {
            return m_Demuxer.Video().Height;
        }
        uint32_t GetAudioChannels() const
        {
            return m_Audio ? m_Audio->Channels() : 0;
        }
        const VideoFrame* GetCurrentFrame() const
        {
            return m_Current ? &*m_Current : nullptr;
        }
        uint64_t FrameSerial() const
        {
            return m_FrameSerial;
        }
        const std::string& Error() const
        {
            return m_Error;
        }

    private:
        int64_t ClockNs() const;
        void    Rewind( int64_t targetNs );
        bool    Pump( int64_t clockNs );
        void    Present( int64_t clockNs );
        void    Fail( std::string error );

        WebmDemuxer                       m_Demuxer;
        std::unique_ptr<Av1VideoDecoder>  m_Video;
        std::unique_ptr<OpusAudioDecoder> m_Audio;
        IMediaAudioSink*                  m_Sink = nullptr;

        MediaPlayerState          m_State             = MediaPlayerState::Closed;
        bool                      m_Looping           = false;
        int64_t                   m_DurationNs        = 0;
        int64_t                   m_OriginNs          = 0; // media time of the sink's played-frame 0
        int64_t                   m_InternalNs        = 0; // the clock when there is no sink
        int64_t                   m_DropAudioBeforeNs = 0;
        bool                      m_DemuxDone         = false;
        std::deque<VideoFrame>    m_Queue; // decoded, not yet shown, in presentation order
        std::optional<VideoFrame> m_Current;
        uint64_t                  m_FrameSerial = 0;
        std::vector<VideoFrame>   m_Decoded; // scratch
        std::vector<float>        m_Pcm;     // scratch
        std::string               m_Error;
    };
} // namespace Desert::Media
