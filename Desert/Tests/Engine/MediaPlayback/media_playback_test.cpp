// MediaPlayback — Engine/Media against a clip whose content is known by construction.
//
// The clip (Desert/Tests/Data/Media/red_440hz_1s.webm) was made by ffmpeg with SVT-AV1 and libopus:
//   ffmpeg -f lavfi -i "color=c=red:s=320x180:r=30:d=1" -f lavfi -i
//   "sine=frequency=440:sample_rate=48000:duration=1"
//          -c:v libsvtav1 -preset 8 -pix_fmt yuv420p -c:a libopus -b:a 64k -ac 2 -shortest red_440hz_1s.webm
// so: 30 frames of solid red (BT.601 limited range: Y' 81, Cb 90, Cr 240) and exactly 48 000 stereo frames
// of sound once Opus's pre-skip and the last packet's DiscardPadding are dropped.

#include <Engine/Media/MediaPlayer.hpp>

#include <gtest/gtest.h>

#include <cstdlib>

using namespace Desert::Media;

namespace
{
    // An audio "device" the test drives: what is pushed is queued, Consume() plays it.
    class CountingSink final : public IMediaAudioSink
    {
    public:
        std::string Start( uint32_t sampleRate, uint32_t channels ) override
        {
            SampleRate = sampleRate;
            Channels   = channels;
            return {};
        }
        void Push( const float*, uint64_t frames ) override
        {
            Queued += frames;
            Pushed += frames;
        }
        uint64_t PlayedFrames() const override
        {
            return Played;
        }
        uint64_t QueuedFrames() const override
        {
            return Queued;
        }
        void SetPaused( bool paused ) override
        {
            Paused = paused;
        }
        void Flush() override
        {
            Queued = 0;
            Played = 0;
        }
        void Consume( uint64_t frames )
        {
            const uint64_t n = frames < Queued ? frames : Queued;
            Queued -= n;
            Played += n;
        }

        uint32_t SampleRate = 0, Channels = 0;
        uint64_t Queued = 0, Played = 0, Pushed = 0;
        bool     Paused = true;
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
    player.SetAudioSink( &sink );
    ASSERT_EQ( player.Open( Clip() ), "" );
    EXPECT_EQ( sink.SampleRate, 48000u );
    EXPECT_EQ( sink.Channels, 2u );
    int ends            = 0;
    player.OnEndReached = [&] { ++ends; };
    player.Play();
    EXPECT_FALSE( sink.Paused );

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
    EXPECT_EQ( sink.Pushed, 48000u );
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

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
