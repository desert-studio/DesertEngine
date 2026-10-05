// MediaPlayback — Engine/Media against a clip whose content is known by construction.
//
// The clip (Desert/Tests/Data/Media/red_440hz_1s.webm) was made by ffmpeg with SVT-AV1 and libopus:
//   ffmpeg -f lavfi -i "color=c=red:s=320x180:r=30:d=1" -f lavfi -i
//   "sine=frequency=440:sample_rate=48000:duration=1"
//          -c:v libsvtav1 -preset 8 -pix_fmt yuv420p -c:a libopus -b:a 64k -ac 2 -shortest red_440hz_1s.webm
// so: 30 frames of solid red (BT.601 limited range: Y' 81, Cb 90, Cr 240) and exactly 48 000 stereo frames
// of sound once Opus's pre-skip and the last packet's DiscardPadding are dropped.
//
// The second clip (testsrc2_1080p_5s.webm) is a MOVING picture — the solid one cannot tell a frozen decoder
// from a working one:
//   ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=5 -f lavfi -i
//   sine=frequency=440:sample_rate=48000:duration=5 -c:v libsvtav1 -preset 8 -crf 50 -pix_fmt yuv420p
//          -c:a libopus -b:a 48k -shortest testsrc2_1080p_5s.webm

#include <cstring>
#include <Engine/Media/MediaPlayer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>

using namespace Desert::Media;

namespace
{
    // An audio "device" the test drives: what is pushed is queued, Consume() plays it. Thread-safe like a
    // real sink: the player's decode thread pushes while the test consumes and reads.
    class CountingSink final : public IMediaAudioSink
    {
    public:
        std::string Start( uint32_t sampleRate, uint32_t channels ) override
        {
            const std::lock_guard lock( m_Lock );
            SampleRate = sampleRate;
            Channels   = channels;
            return {};
        }
        void Push( const float*, uint64_t frames ) override
        {
            {
                const std::lock_guard lock( m_Lock );
                m_Queued += frames;
                m_Pushed += frames;
            }
            m_Fed.notify_all();
        }
        uint64_t PlayedFrames() const override
        {
            const std::lock_guard lock( m_Lock );
            return m_Played;
        }
        uint64_t QueuedFrames() const override
        {
            const std::lock_guard lock( m_Lock );
            return m_Queued;
        }
        void SetPaused( bool paused ) override
        {
            const std::lock_guard lock( m_Lock );
            m_Paused = paused;
        }
        void Flush() override
        {
            const std::lock_guard lock( m_Lock );
            m_Queued            = 0;
            m_Played            = 0;
            m_PushedBeforeFlush = m_Pushed; // one pass's sound: a flush ends a pass (the end, a seek)
            m_Pushed            = 0;
        }
        // A device pulls its period: it waits for the decode thread to feed it (as a real output's callback is
        // fed ahead), and plays short only when nothing more comes (the stream's end).
        void Consume( uint64_t frames )
        {
            std::unique_lock lock( m_Lock );
            m_Fed.wait_for( lock, std::chrono::milliseconds( 200 ), [&] { return m_Queued >= frames; } );
            const uint64_t n = frames < m_Queued ? frames : m_Queued;
            m_Queued -= n;
            m_Played += n;
        }
        bool Paused() const
        {
            const std::lock_guard lock( m_Lock );
            return m_Paused;
        }
        uint64_t PushedBeforeFlush() const
        {
            const std::lock_guard lock( m_Lock );
            return m_PushedBeforeFlush;
        }

        uint32_t SampleRate = 0, Channels = 0;

    private:
        mutable std::mutex      m_Lock;
        std::condition_variable m_Fed;
        uint64_t                m_Queued = 0, m_Played = 0, m_Pushed = 0, m_PushedBeforeFlush = 0;
        bool                    m_Paused = true;
    };

    MediaSource Clip()
    {
        return MediaSource{ DESERT_MEDIA_TEST_CLIP };
    }

    uint8_t SampleAt( const VideoFrame& f, int plane, uint32_t x, uint32_t y )
    {
        return f.Planes[plane][static_cast<size_t>( y ) * f.PlaneWidth[plane] + x];
    }
} // namespace

TEST( MediaPlayback, OpenDecodesTheFirstFrameAsPlanes )
{
    MediaPlayer player;
    ASSERT_EQ( player.Open( Clip() ), "" );
    EXPECT_EQ( player.GetState(), MediaPlayerState::Stopped );
    EXPECT_TRUE( player.HasVideo() );
    EXPECT_TRUE( player.HasAudio() );
    EXPECT_EQ( player.GetVideoWidth(), 320u );
    EXPECT_EQ( player.GetVideoHeight(), 180u );

    const VideoFrame* frame = player.GetCurrentFrame();
    ASSERT_NE( frame, nullptr );
    EXPECT_EQ( frame->PtsNs, 0 );
    EXPECT_EQ( frame->Width, 320u );
    EXPECT_EQ( frame->Height, 180u );
    EXPECT_EQ( frame->BitDepth, 8u );
    EXPECT_EQ( frame->Chroma, MediaChroma::I420 );
    EXPECT_EQ( frame->PlaneWidth[1], 160u );
    EXPECT_EQ( frame->PlaneHeight[1], 90u );
    // Solid red, untouched by any RGB conversion (that is the GPU's job): Y'CbCr as the encoder got it.
    EXPECT_NEAR( SampleAt( *frame, 0, 160, 90 ), 81, 6 );
    EXPECT_NEAR( SampleAt( *frame, 1, 80, 45 ), 90, 6 );
    EXPECT_NEAR( SampleAt( *frame, 2, 80, 45 ), 240, 6 );
}

TEST( MediaPlayback, DurationIsTheContainers )
{
    MediaPlayer player;
    ASSERT_EQ( player.Open( Clip() ), "" );
    // ffprobe: format duration 1.008 s (the audio's last packet ends there before its padding is cut).
    EXPECT_NEAR( player.GetDuration(), 1.008, 0.002 );
    EXPECT_DOUBLE_EQ( player.GetTime(), 0.0 );
}

TEST( MediaPlayback, EndReachedFiresExactlyOnceAndEveryFrameIsShown )
{
    MediaPlayer player;
    player.SetBlockOnTime( true ); // the clock runs faster than real time here
    ASSERT_EQ( player.Open( Clip() ), "" );
    int      ends        = 0;
    uint64_t serialAtEnd = 0;
    player.OnEndReached  = [&]
    {
        ++ends;
        serialAtEnd = player.FrameSerial();
    };
    const uint64_t firstSerial = player.FrameSerial();
    player.Play();
    for ( int i = 0; i < 180; ++i ) // three seconds at 60 Hz: well past the end
        player.Tick( 1.0 / 60.0 );
    EXPECT_EQ( ends, 1 );
    EXPECT_EQ( player.GetState(), MediaPlayerState::Stopped );
    // 30 frames at 30 fps ticked at 60 Hz: none needs skipping, so each one became current exactly once.
    EXPECT_EQ( serialAtEnd - firstSerial, 29u );
}

TEST( MediaPlayback, LoopingRewindsAndKeepsPlaying )
{
    MediaPlayer player;
    player.SetBlockOnTime( true );
    ASSERT_EQ( player.Open( Clip() ), "" );
    player.SetLooping( true );
    int ends            = 0;
    player.OnEndReached = [&] { ++ends; };
    player.Play();
    for ( int i = 0; i < 90; ++i ) // 1.5 s
        player.Tick( 1.0 / 60.0 );
    EXPECT_EQ( ends, 1 );
    EXPECT_EQ( player.GetState(), MediaPlayerState::Playing );
    EXPECT_LT( player.GetTime(), 0.6 );
    ASSERT_NE( player.GetCurrentFrame(), nullptr );
    EXPECT_LT( player.GetCurrentFrame()->PtsNs, 600000000 );
}

TEST( MediaPlayback, SoundDecodesToTheExpectedSampleCountAndDrivesTheClock )
{
    CountingSink sink;
    MediaPlayer  player;
    player.SetBlockOnTime( true );
    player.SetAudioSink( &sink );
    ASSERT_EQ( player.Open( Clip() ), "" );
    EXPECT_EQ( sink.SampleRate, 48000u );
    EXPECT_EQ( sink.Channels, 2u );
    int ends            = 0;
    player.OnEndReached = [&] { ++ends; };
    player.Play();
    EXPECT_FALSE( sink.Paused() );

    // The clock is the sink's played count: half a second played ⇒ the picture is half a second in.
    for ( int i = 0; i < 30; ++i )
    {
        sink.Consume( 800 ); // 1/60 s
        player.Tick( 0.0 );  // the delta is ignored when sound leads
    }
    EXPECT_NEAR( player.GetTime(), 0.5, 1e-6 );
    ASSERT_NE( player.GetCurrentFrame(), nullptr );
    EXPECT_NEAR( static_cast<double>( player.GetCurrentFrame()->PtsNs ) * 1e-9, 0.5, 1.0 / 30.0 + 1e-6 );

    for ( int i = 0; i < 120 && ends == 0; ++i )
    {
        sink.Consume( 800 );
        player.Tick( 0.0 );
    }
    EXPECT_EQ( ends, 1 );
    EXPECT_EQ( sink.PushedBeforeFlush(), 48000u ); // the end flushed the pass
}

TEST( MediaPlayback, SeekLandsOnTheFrameAtTheTarget )
{
    MediaPlayer player;
    ASSERT_EQ( player.Open( Clip() ), "" );
    ASSERT_TRUE( player.Seek( 0.5 ) ) << player.Error();
    EXPECT_NEAR( player.GetTime(), 0.5, 1e-9 );
    ASSERT_NE( player.GetCurrentFrame(), nullptr );
    const int64_t pts = player.GetCurrentFrame()->PtsNs;
    EXPECT_LE( pts, 500000000 );
    EXPECT_GT( pts, 500000000 - 34000000 );
}

// A moving clip: frames one second apart differ, so the picture a MediaTexture uploads actually changes.
TEST( MediaPlayback, PatternClipFramesOneSecondApartDiffer )
{
    MediaPlayer player;
    player.SetBlockOnTime( true );
    ASSERT_EQ( player.Open( MediaSource{ DESERT_MEDIA_PATTERN_CLIP } ), "" );
    ASSERT_NE( player.GetCurrentFrame(), nullptr );
    EXPECT_EQ( player.GetCurrentFrame()->Width, 1920u );
    EXPECT_EQ( player.GetCurrentFrame()->Height, 1080u );
    const MediaPlane&          plane0 = player.GetCurrentFrame()->Planes[0];
    const std::vector<uint8_t> first( plane0.data(), plane0.data() + plane0.size() );
    const uint64_t             serial = player.FrameSerial();
    player.Play();
    for ( int i = 0; i < 30; ++i )
        player.Tick( 1.0 / 30.0 );
    ASSERT_NE( player.GetCurrentFrame(), nullptr );
    EXPECT_GT( player.FrameSerial(), serial );
    const MediaPlane& now = player.GetCurrentFrame()->Planes[0];
    EXPECT_NE( std::vector<uint8_t>( now.data(), now.data() + now.size() ), first );
}

// Decode throughput of the C path: every frame of the clip made current once (ticked at its own frame
// rate), wall time per frame printed. The pattern clip always; DESERT_MEDIA_BENCH_CLIP adds one more file
// (the 1080p60 / 4K60 measurements are made with it on a clip outside the repository).
TEST( MediaPlayback, APlaneIsSizedWithoutAPassOverItsBytesAndComparesByContent )
{
    // MEDIA-5: a decoded plane is overwritten whole, so sizing it must not touch its bytes (a vector's resize
    // and destruction walked 12 MB per 4K plane on Debug), and re-sizing to the same size keeps the buffer.
    MediaPlane a;
    EXPECT_TRUE( a.empty() );
    a.Allocate( 16 );
    const uint8_t* buffer = a.data();
    a.Allocate( 16 );
    EXPECT_EQ( a.data(), buffer );
    EXPECT_EQ( a.size(), 16u );
    std::memset( a.data(), 7, a.size() );
    MediaPlane b;
    b.Allocate( 16 );
    std::memset( b.data(), 7, b.size() );
    EXPECT_TRUE( a == b );
    b.data()[15] = 8;
    EXPECT_FALSE( a == b );
    EXPECT_EQ( b[15], 8u );
}

TEST( MediaPlayback, DecodeThroughputIsPrinted )
{
    const std::vector<std::string> clips{ DESERT_MEDIA_PATTERN_CLIP
#ifdef DESERT_MEDIA_BENCH_CLIP // a second, heavier clip: build with -DDESERT_MEDIA_BENCH_CLIP='"<path>"'
                                          ,
                                          DESERT_MEDIA_BENCH_CLIP
#endif
    };
    for ( const std::string& clip : clips )
    {
        MediaPlayer player;
        player.SetBlockOnTime( true ); // every frame made current: the decode thread's throughput
        ASSERT_EQ( player.Open( MediaSource{ clip } ), "" ) << clip;
        ASSERT_NE( player.GetCurrentFrame(), nullptr ) << clip;
        const uint32_t width       = player.GetCurrentFrame()->Width;
        const uint32_t height      = player.GetCurrentFrame()->Height;
        bool           ended       = false;
        player.OnEndReached        = [&] { ended = true; };
        const uint64_t firstSerial = player.FrameSerial();
        player.Play();
        const auto start = std::chrono::steady_clock::now();
        for ( int i = 0; i < 100000 && !ended; ++i )
            player.Tick( 1.0 / 120.0 ); // finer than any clip's frame time: no frame is skipped
        const double   seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
        const uint64_t frames  = player.FrameSerial() - firstSerial;
        ASSERT_TRUE( ended ) << clip;
        ASSERT_GT( frames, 0u ) << clip;
        std::printf( "[decode] %s: %ux%u, %llu frames in %.3f s = %.1f fps\n", clip.c_str(), width, height,
                     static_cast<unsigned long long>( frames ), seconds, static_cast<double>( frames ) / seconds );
    }
}

// Without SetBlockOnTime a Tick only takes what the decode thread has ready: the clock may run ahead of
// the decoder (frames are then skipped, never waited for), and the clip still reaches its end once.
TEST( MediaPlayback, TickWithoutBlockOnTimeNeverWaitsAndStillReachesTheEnd )
{
    MediaPlayer player;
    ASSERT_EQ( player.Open( MediaSource{ DESERT_MEDIA_PATTERN_CLIP } ), "" );
    ASSERT_NE( player.GetCurrentFrame(), nullptr ); // the open is a preroll: the first frame is there
    int ends            = 0;
    player.OnEndReached = [&] { ++ends; };
    player.Play();
    player.Tick( 10.0 ); // past the 5 s clip at once
    const auto start = std::chrono::steady_clock::now();
    while ( ends == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds( 30 ) )
        player.Tick( 0.0 );
    EXPECT_EQ( ends, 1 );
    EXPECT_EQ( player.GetState(), MediaPlayerState::Stopped );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
