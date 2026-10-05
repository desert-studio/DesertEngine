// MOVIE-AUDIO-a: a UI clip's Audio track (UE's Sequencer Audio track) — the section's sound under the
// playhead (Evaluator), and the voice diff that turns frames into Start / Seek / SetGain / Stop on the
// frames they belong to (AudioVoices).

#include "TimelineFixtures.hpp"

#include <Engine/Animation/Timeline/AudioVoices.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace Desert::Animation;
using namespace Desert::Animation::Timeline;
using namespace TimelineFixtures;

namespace
{
    // A UI clip at 100 ticks/s: one Audio track on the master binding, one section [100, 300] playing
    // the Title sound at volume 0.5 with a 50-tick fade-in and a 50-tick fade-out.
    // `.desound` GUIDs: a section names a sound's identity, never its file (SoundAsset).
    const Common::Content::AssetGuid kTitleSound{ 0x5A0D, 0x0001 };
    const Common::Content::AssetGuid kStudioSound{ 0x5A0D, 0x0002 };
    const Common::Content::AssetGuid kWindSound{ 0x5A0D, 0x0003 };

    Sequence UIClipWithSound()
    {
        Sequence sequence;
        sequence.Host     = SequenceHost::UIAnimation;
        sequence.TickRate = FrameRate{ 100, 1 };
        sequence.Start    = Tick( 0 );
        sequence.End      = Tick( 400 );
        sequence.Bindings.push_back( Binding{ Guid( 1 ), BindingKind::Sequence, "", "Clip", {} } );
        Track track;
        track.Binding  = Guid( 1 );
        track.Property = "Music";
        track.Kind     = TrackKind::Audio;
        Section section;
        section.Start   = Tick( 100 );
        section.End     = Tick( 300 );
        section.Content = AudioSectionContent{ kTitleSound, Tick( 0 ), 0.5F, Tick( 50 ), Tick( 50 ) };
        track.Sections.push_back( std::move( section ) );
        sequence.Tracks.push_back( std::move( track ) );
        return sequence;
    }

    std::vector<AudioSample> SoundsAt( const Sequence& sequence, const int32_t tick )
    {
        EvaluatedFrame frame;
        Evaluator( sequence ).Evaluate( TimeStep{ At( tick ), At( tick ) }, frame );
        return frame.Sounds;
    }

    std::vector<SoundingVoice> Frame( const Sequence& sequence, const int32_t tick, const double advanced )
    {
        std::vector<SoundingVoice> voices;
        for ( const AudioSample& s : SoundsAt( sequence, tick ) )
            voices.push_back( SoundingVoice{ 7, s.TrackIndex, s.SectionIndex, s.Sound, s.SoundSeconds, advanced,
                                             s.Gain } );
        return voices;
    }
} // namespace

TEST( TimelineAudio, UIClipHoldsAudioOnItsMasterBindingOnly )
{
    Sequence sequence = UIClipWithSound();
    EXPECT_TRUE( Validate( sequence ).IsSuccess() );

    Sequence onWidget = sequence;
    onWidget.Bindings.push_back( Binding{ Guid( 2 ), BindingKind::Widget, "5007", "Rim", {} } );
    onWidget.Tracks[0].Binding = Guid( 2 );
    EXPECT_FALSE( Validate( onWidget ).IsSuccess() ) << "a sound binds no widget";

    Sequence silent = sequence;
    std::get<AudioSectionContent>( silent.Tracks[0].Sections[0].Content ).Sound = Common::Content::AssetGuid{};
    EXPECT_FALSE( Validate( silent ).IsSuccess() ) << "an Audio section names its sound";
}

TEST( TimelineAudio, SectionMapsThePlayheadIntoTheSoundWithFades )
{
    const Sequence sequence = UIClipWithSound();
    EXPECT_TRUE( SoundsAt( sequence, 99 ).empty() );
    EXPECT_TRUE( SoundsAt( sequence, 301 ).empty() );

    const auto fadingIn = SoundsAt( sequence, 125 );
    ASSERT_EQ( fadingIn.size(), 1u );
    EXPECT_EQ( fadingIn[0].Sound, kTitleSound );
    EXPECT_DOUBLE_EQ( fadingIn[0].SoundSeconds, 0.25 );
    EXPECT_FLOAT_EQ( fadingIn[0].Gain, 0.25F ); // volume 0.5 × half the fade-in

    const auto full = SoundsAt( sequence, 200 );
    ASSERT_EQ( full.size(), 1u );
    EXPECT_DOUBLE_EQ( full[0].SoundSeconds, 1.0 );
    EXPECT_FLOAT_EQ( full[0].Gain, 0.5F );

    EXPECT_FLOAT_EQ( SoundsAt( sequence, 275 )[0].Gain, 0.25F ); // half the fade-out
}

TEST( TimelineAudio, VoicesFollowThePlayheadFrameByFrame )
{
    const Sequence sequence = UIClipWithSound();
    AudioVoices                voices;
    std::vector<AudioCommand> out;

    voices.Update( Frame( sequence, 50, 0.25 ), out ); // before the section: nothing
    EXPECT_TRUE( out.empty() );

    voices.Update( Frame( sequence, 125, 0.75 ), out ); // the playhead entered: Start where the picture is
    ASSERT_EQ( out.size(), 1u );
    EXPECT_EQ( out[0].Kind, AudioCommandKind::Start );
    EXPECT_EQ( out[0].Sound, kTitleSound );
    EXPECT_DOUBLE_EQ( out[0].Seconds, 0.25 );
    EXPECT_FLOAT_EQ( out[0].Gain, 0.25F );

    out.clear();
    voices.Update( Frame( sequence, 200, 0.75 ), out ); // continuous play: only the fade's gain changes
    ASSERT_EQ( out.size(), 1u );
    EXPECT_EQ( out[0].Kind, AudioCommandKind::SetGain );
    EXPECT_FLOAT_EQ( out[0].Gain, 0.5F );

    out.clear();
    voices.Update( Frame( sequence, 210, 0.10 ), out ); // steady: no command at all
    EXPECT_TRUE( out.empty() );

    out.clear();
    voices.Update( Frame( sequence, 150, 0.0 ), out ); // a scrub / loop wrap back: Seek, the gain unchanged
    ASSERT_EQ( out.size(), 1u );
    EXPECT_EQ( out[0].Kind, AudioCommandKind::Seek );
    EXPECT_DOUBLE_EQ( out[0].Seconds, 0.5 );

    out.clear();
    voices.Update( {}, out ); // paused / stopped (the owner sends nothing): Stop
    ASSERT_EQ( out.size(), 1u );
    EXPECT_EQ( out[0].Kind, AudioCommandKind::Stop );
    EXPECT_EQ( voices.Playing(), 0u );

    out.clear();
    voices.Update( Frame( sequence, 160, 0.1 ), out ); // resumed: Start again at the playhead
    ASSERT_EQ( out.size(), 1u );
    EXPECT_EQ( out[0].Kind, AudioCommandKind::Start );
    EXPECT_DOUBLE_EQ( out[0].Seconds, 0.6 );

    out.clear();
    voices.Update( Frame( sequence, 350, 1.9 ), out ); // past the section's end: Stop
    ASSERT_EQ( out.size(), 1u );
    EXPECT_EQ( out[0].Kind, AudioCommandKind::Stop );
}

TEST( TimelineAudio, AudioSectionRoundTripsThroughTheFormat )
{
    const Sequence sequence = UIClipWithSound();
    auto           bytes    = WriteSequence( sequence );
    ASSERT_TRUE( bytes.IsSuccess() ) << bytes.GetError();
    auto read = ReadSequence( bytes.GetValue() );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const auto& audio = std::get<AudioSectionContent>( read.GetValue().Tracks[0].Sections[0].Content );
    EXPECT_EQ( audio.Sound, kTitleSound );
    EXPECT_FLOAT_EQ( audio.Volume, 0.5F );
    EXPECT_EQ( audio.FadeIn.Value, 50 );
    EXPECT_EQ( audio.FadeOut.Value, 50 );
}

// MOVIE-AUDIO-c: the Sequencer's "+ Track > Audio" — one master binding however many sounds, a track per
// sound named apart, one section over the playback range holding the sound, and the refusals Validate makes.
TEST( TimelineAudio, AddAudioTrackPutsASoundOnTheMasterBindingOverThePlaybackRange )
{
    Sequence sequence;
    sequence.Host  = SequenceHost::UIAnimation;
    sequence.Start = Tick( 0 );
    sequence.End   = Tick( 900 );
    sequence.Bindings.push_back( Binding{ Guid( 2 ), BindingKind::Widget, "5007", "Rim", {} } );
    const uint32_t revision = sequence.Revision;

    const auto first = AddAudioTrack( sequence, kStudioSound );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    const auto second = AddAudioTrack( sequence, kWindSound );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    EXPECT_GT( sequence.Revision, revision );

    ASSERT_EQ( sequence.Bindings.size(), 2u ) << "the master binding is created once, then reused";
    EXPECT_EQ( sequence.Bindings[1].Kind, BindingKind::Sequence );
    const Track& track = sequence.Tracks[first.GetValue()];
    EXPECT_EQ( track.Kind, TrackKind::Audio );
    EXPECT_EQ( track.Binding, sequence.Bindings[1].Guid );
    EXPECT_EQ( track.Property, "Audio" );
    EXPECT_EQ( sequence.Tracks[second.GetValue()].Property, "Audio 2" );
    ASSERT_EQ( track.Sections.size(), 1u );
    EXPECT_EQ( track.Sections[0].Start.Value, 0 );
    EXPECT_EQ( track.Sections[0].End.Value, 900 );
    EXPECT_EQ( std::get<AudioSectionContent>( track.Sections[0].Content ).Sound,
               kStudioSound );
    EXPECT_TRUE( Validate( sequence ).IsSuccess() ) << Validate( sequence ).GetError();

    const Sequence before = sequence;
    EXPECT_FALSE( AddAudioTrack( sequence, Common::Content::AssetGuid{} ).IsSuccess() ) << "an Audio section names its sound";
    EXPECT_EQ( sequence.Tracks.size(), before.Tracks.size() ) << "a refusal writes nothing";

    Sequence level;
    level.Host = SequenceHost::LevelSequence;
    EXPECT_FALSE( AddAudioTrack( level, kWindSound ).IsSuccess() ) << "a level sequence plays no sound yet";
    EXPECT_TRUE( level.Bindings.empty() && level.Tracks.empty() );
}
