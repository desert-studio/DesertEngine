// THE TIMELINE CORE'S CONTRACT, group 3 without the fold: WeightAt, AddSection, Find*, Validate and the
// host's restrictions (ANIM-I23: Section/Track/Sequence.cpp). The fold itself needs the Evaluator (I6) and
// lives in timeline_evaluator_test.cpp.

#include "TimelineFixtures.hpp"

using namespace TimelineFixtures;

// ── 3. Sections, tracks, the sequence ───────────────────────────────────────────────────────────────

TEST( TimelineSection, AnEmptyWeightIsOneNotZero )
{
    Section section;
    EXPECT_EQ( WeightAt( section, At( 5 ), FrameRate{ 60, 1 } ), 1.0F );
}

TEST( TimelineSequence, AMinimalSequenceValidates )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    const auto     valid    = Validate( sequence );
    EXPECT_TRUE( valid.IsSuccess() ) << valid.GetError();
}

TEST( TimelineSequence, ValidateRefusesEachBrokenInvariant )
{
    const Sequence good = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );

    Sequence unknownBinding          = good;
    unknownBinding.Tracks[0].Binding = Guid( 99 );
    EXPECT_FALSE( Validate( unknownBinding ).IsSuccess() ) << "a track pointing at no binding";

    Sequence kindMismatch       = good;
    kindMismatch.Tracks[0].Kind = TrackKind::Vector;
    EXPECT_FALSE( Validate( kindMismatch ).IsSuccess() ) << "a Vector track holding a Float section";

    Sequence duplicate = good;
    duplicate.Tracks.push_back( duplicate.Tracks[0] );
    EXPECT_FALSE( Validate( duplicate ).IsSuccess() ) << "two tracks for one property";

    Sequence unsorted = OneFloatTrack( FloatChannel{ { Key( 20, 1.0F ), Key( 10, 2.0F ) }, 0.0F } );
    EXPECT_FALSE( Validate( unsorted ).IsSuccess() ) << "keys out of tick order";

    Sequence nullGuid         = good;
    nullGuid.Bindings[0].Guid = BindingGuid{};
    EXPECT_FALSE( Validate( nullGuid ).IsSuccess() ) << "a null binding GUID";

    Sequence boolLinear       = good;
    boolLinear.Tracks[0].Kind = TrackKind::Bool;
    boolLinear.Tracks[0].Sections[0].Content =
         Channel{ BoolChannel{ FloatChannel{ { Key( 0, 0.0F ), Key( 10, 1.0F, KeyInterp::Linear ) }, 0.0F } } };
    EXPECT_FALSE( Validate( boolLinear ).IsSuccess() ) << "a Bool key that interpolates";

    Sequence boolAdditive       = good;
    boolAdditive.Tracks[0].Kind = TrackKind::Bool;
    boolAdditive.Tracks[0].Sections[0].Content =
         Channel{ BoolChannel{ FloatChannel{ { Key( 0, 1.0F, KeyInterp::Constant ) }, 0.0F } } };
    boolAdditive.Tracks[0].Sections[0].Blend = SectionBlendType::Additive;
    EXPECT_FALSE( Validate( boolAdditive ).IsSuccess() ) << "an Additive section on a Bool track (a flag has no additive)";

    Sequence rotation       = good;
    rotation.Tracks[0].Kind = TrackKind::Rotation;
    RotationChannel misaligned;
    misaligned.X.Keys                      = { Key( 0, 0.0F ), Key( 10, 0.0F ) };
    misaligned.W.Keys                      = { Key( 0, 1.0F ) };
    rotation.Tracks[0].Sections[0].Content = Channel{ misaligned };
    EXPECT_FALSE( Validate( rotation ).IsSuccess() ) << "rotation components keyed apart";

    Sequence badRange                    = good;
    badRange.Tracks[0].Sections[0].Start = Tick( 50 );
    badRange.Tracks[0].Sections[0].End   = Tick( 10 );
    EXPECT_FALSE( Validate( badRange ).IsSuccess() ) << "Start after End";
}

TEST( TimelineSequence, TheHostRestrictsWhatASequenceMayHold )
{
    Sequence clip = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    clip.Host     = SequenceHost::AnimationClip;
    EXPECT_FALSE( Validate( clip ).IsSuccess() ) << "an .anim cannot bind an entity";

    Sequence ui = clip;
    ui.Host     = SequenceHost::UIAnimation;
    EXPECT_FALSE( Validate( ui ).IsSuccess() ) << "a UI clip binds widgets only";
    ui.Bindings = { Binding{ Guid( 1 ), BindingKind::Widget, "panel-uuid", "Panel", {} },
                    Binding{ Guid( 2 ), BindingKind::Widget, "label-uuid", "Label", {} } };
    EXPECT_TRUE( Validate( ui ).IsSuccess() ) << "a UI clip binds any number of widgets of its tree";

    Sequence level = clip;
    level.Host     = SequenceHost::LevelSequence;
    EXPECT_TRUE( Validate( level ).IsSuccess() );
}

TEST( TimelineSection, AKeyedWeightIsSampledLikeAChannelAndNotClamped )
{
    Section section;
    section.Weight = { Key( 0, 0.0F ), Key( 100, 2.0F ) };
    const FrameRate rate{ 60, 1 };
    EXPECT_EQ( WeightAt( section, At( 50 ), rate ), 1.0F );
    EXPECT_EQ( WeightAt( section, At( 200 ), rate ), 2.0F ) << "held after the last key, and > 1 is legal";
    EXPECT_EQ( WeightAt( section, At( -10 ), rate ), 0.0F ) << "held before the first key";
}

TEST( TimelineTrack, AddSectionHoldsTheTracksKindAndTakesTheFirstFreeRow )
{
    Track track;
    track.Kind           = TrackKind::Rotation;
    const Section& first = AddSection( track, Tick( 0 ), Tick( 50 ) );
    EXPECT_EQ( first.Row, 0 );
    EXPECT_EQ( TrackKindOf( first.Content ), TrackKind::Rotation );
    EXPECT_EQ( std::get<RotationChannel>( std::get<Channel>( first.Content ) ).W.Default, 1.0F );
    EXPECT_TRUE( first.Weight.empty() );

    EXPECT_EQ( AddSection( track, Tick( 40 ), Tick( 90 ) ).Row, 1 ) << "overlaps row 0";
    EXPECT_EQ( AddSection( track, Tick( 60 ), Tick( 70 ) ).Row, 0 ) << "row 0 is free there";

    Track cuts;
    cuts.Kind = TrackKind::CameraCut;
    EXPECT_EQ( TrackKindOf( AddSection( cuts, Tick( 0 ), Tick( 10 ) ).Content ), TrackKind::CameraCut );
    Track animation;
    animation.Kind = TrackKind::Animation;
    EXPECT_EQ( TrackKindOf( AddSection( animation, Tick( 0 ), Tick( 10 ) ).Content ), TrackKind::Animation );
}

TEST( TimelineSequence, FindBindingAndFindTrackAnswerByGuidAndProperty )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    ASSERT_NE( FindBinding( sequence, Guid( 1 ) ), nullptr );
    EXPECT_EQ( FindBinding( sequence, Guid( 1 ) )->Label, "Door" );
    EXPECT_EQ( FindBinding( sequence, Guid( 2 ) ), nullptr );
    EXPECT_EQ( FindTrack( sequence, Guid( 1 ), "Opacity" ), &sequence.Tracks[0] );
    EXPECT_EQ( FindTrack( sequence, Guid( 1 ), "Color" ), nullptr );
    EXPECT_EQ( FindTrack( sequence, Guid( 2 ), "Opacity" ), nullptr );
}

TEST( TimelineSequence, TheErrorNamesTheTrackAndTheSection )
{
    Sequence   sequence = OneFloatTrack( FloatChannel{ { Key( 20, 1.0F ), Key( 10, 2.0F ) }, 0.0F } );
    const auto valid    = Validate( sequence );
    ASSERT_FALSE( valid.IsSuccess() );
    EXPECT_NE( valid.GetError().find( "track 'Door' / 'Opacity': section 0:" ), std::string::npos )
         << valid.GetError();
}

TEST( TimelineSequence, ALevelSequenceBoneSitsUnderItsEntity )
{
    Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    sequence.Bindings.push_back( Binding{ Guid( 2 ), BindingKind::Bone, "Hand_L", "Hand_L", {} } );
    EXPECT_FALSE( Validate( sequence ).IsSuccess() ) << "a bone with no entity above it";
    sequence.Bindings[1].Parent = Guid( 1 );
    EXPECT_TRUE( Validate( sequence ).IsSuccess() );

    sequence.Bindings[0].Parent = Guid( 2 );
    EXPECT_FALSE( Validate( sequence ).IsSuccess() ) << "a parent cycle";
}

TEST( TimelineSequence, ACameraCutIsAMasterTrackWhoseCutsNeverOverlapOnARow )
{
    Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    sequence.Bindings.push_back( Binding{ Guid( 3 ), BindingKind::Sequence, "", "Master", {} } );
    Track cuts;
    cuts.Binding = Guid( 3 );
    cuts.Kind    = TrackKind::CameraCut;
    AddSection( cuts, Tick( 0 ), Tick( 40 ) );
    AddSection( cuts, Tick( 41 ), Tick( 100 ) );
    for ( Section& section : cuts.Sections )
    {
        std::get<CameraCutSectionContent>( section.Content ).Camera = Guid( 1 );
    }
    sequence.Tracks.push_back( cuts );
    const auto valid = Validate( sequence );
    EXPECT_TRUE( valid.IsSuccess() ) << valid.GetError();

    Sequence overlap                    = sequence;
    overlap.Tracks[1].Sections[1].Start = Tick( 30 );
    EXPECT_FALSE( Validate( overlap ).IsSuccess() ) << "two cameras on one row at tick 30";

    Sequence weighted                     = sequence;
    weighted.Tracks[1].Sections[0].Weight = { Key( 0, 0.5F ) };
    EXPECT_FALSE( Validate( weighted ).IsSuccess() ) << "a cut is Absolute at full weight";

    Sequence onEntity          = sequence;
    onEntity.Tracks[1].Binding = Guid( 1 );
    EXPECT_FALSE( Validate( onEntity ).IsSuccess() ) << "Camera Cut belongs to the sequence, not an entity";

    Sequence clip = sequence;
    clip.Host     = SequenceHost::AnimationClip;
    EXPECT_FALSE( Validate( clip ).IsSuccess() ) << "a .anim holds no Camera Cut";
}

TEST( TimelineSequence, AClipHoldsBoneTransformsCurvesAndOneNotifyTrack )
{
    Sequence clip;
    clip.Host = SequenceHost::AnimationClip;
    clip.Bindings.push_back( Binding{ Guid( 1 ), BindingKind::Bone, "Hand_L", "Hand_L", {} } );
    clip.Bindings.push_back( Binding{ Guid( 2 ), BindingKind::Sequence, "", "Clip", {} } );
    Track bone{ Guid( 1 ), "", TrackKind::Transform, {}, false };
    AddSection( bone, Tick( 0 ), Tick( 10 ) );
    Track curve{ Guid( 2 ), "Footstep_L", TrackKind::Float, {}, false };
    AddSection( curve, Tick( 0 ), Tick( 10 ) );
    Track notifies{ Guid( 2 ), "", TrackKind::Event, {}, false };
    AddSection( notifies, Tick( 0 ), Tick( 10 ) );
    clip.Tracks      = { bone, curve, notifies };
    const auto valid = Validate( clip );
    EXPECT_TRUE( valid.IsSuccess() ) << valid.GetError();

    Sequence twoNotifyTracks = clip;
    twoNotifyTracks.Tracks.push_back( notifies );
    twoNotifyTracks.Tracks.back().Property = "More";
    EXPECT_FALSE( Validate( twoNotifyTracks ).IsSuccess() );

    Sequence boneFloat           = clip;
    boneFloat.Tracks[0].Kind     = TrackKind::Float;
    boneFloat.Tracks[0].Sections = {};
    EXPECT_FALSE( Validate( boneFloat ).IsSuccess() ) << "a bone takes a transform only";

    Sequence sameBoneTwice = clip;
    sameBoneTwice.Bindings.push_back( Binding{ Guid( 3 ), BindingKind::Bone, "Hand_L", "Hand_L copy", {} } );
    EXPECT_FALSE( Validate( sameBoneTwice ).IsSuccess() );

    Sequence cubicRotation = clip;
    auto&    transform =
         std::get<TransformChannel>( std::get<Channel>( cubicRotation.Tracks[0].Sections[0].Content ) );
    for ( FloatChannel* component :
          { &transform.Rotation.X, &transform.Rotation.Y, &transform.Rotation.Z, &transform.Rotation.W } )
    {
        component->Keys = { Key( 0, 0.0F, KeyInterp::Cubic ) };
    }
    EXPECT_FALSE( Validate( cubicRotation ).IsSuccess() ) << "Cubic rotation waits for squad";
}
