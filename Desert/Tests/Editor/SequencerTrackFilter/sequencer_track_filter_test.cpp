// THE SEQUENCER'S TRACK FILTERS (Selected, Keyed), ASSERTED WITHOUT A WINDOW.
//
// The rule lives in Editor/Panels/Sequencer/TrackFilter.hpp; the timeline only asks it. What is pinned here is
// UE's behaviour (SequencerTrackFilter_Selected.cpp, _Keyed.cpp):
//   * no active filter shows everything — the filters are subtractive and off by default;
//   * Selected is about the ACTOR: a selected binding shows every track, keyed or not; an unselected one shows
//   none;
//   * Keyed is about the TRACK: any key in any channel component, or in a section's weight, makes it pass; an
//     empty channel and a Camera Cut do not; a binding passes when any of ITS tracks does — another binding's
//     keys never lend it a row;
//   * both on is AND.

#include <Editor/Panels/Sequencer/TrackFilter.hpp>

#include <Engine/Animation/Timeline/Sequence.hpp>

#include <gtest/gtest.h>

namespace T = Desert::Animation::Timeline;
using Desert::Editor::Sequencer::BindingPasses;
using Desert::Editor::Sequencer::TrackFilters;
using Desert::Editor::Sequencer::TrackPasses;

namespace
{
    T::Binding Actor( const char* locator )
    {
        T::Binding b;
        b.Kind    = T::BindingKind::Entity;
        b.Locator = locator;
        b.Guid    = T::BindingGuid::ForObject( b.Kind, b.Locator );
        b.Label   = locator;
        return b;
    }

    T::Track ChannelTrack( const T::BindingGuid& binding, const char* property, T::Channel channel )
    {
        T::Track track;
        track.Binding  = binding;
        track.Property = property;
        track.Kind     = static_cast<T::TrackKind>( T::KindOf( channel ) );
        T::Section section;
        section.Start   = Desert::Animation::FrameNumber{ 0 };
        section.End     = Desert::Animation::FrameNumber{ 100 };
        section.Content = std::move( channel );
        track.Sections.push_back( std::move( section ) );
        return track;
    }

    Desert::Animation::ScalarKey Key( int32_t tick, float value )
    {
        Desert::Animation::ScalarKey k;
        k.Tick  = Desert::Animation::FrameNumber{ tick };
        k.Value = value;
        return k;
    }

    T::Channel KeyedTransform()
    {
        T::TransformChannel c;
        c.Rotation.W.Keys.push_back( Key( 10, 1.0F ) ); // only ONE component of one part keyed
        return c;
    }
} // namespace

TEST( SequencerTrackFilter, HasKeysSeesAnyComponentAndTheSectionWeight )
{
    const T::BindingGuid guid = Actor( "a" ).Guid;
    EXPECT_FALSE( T::TrackHasKeys( ChannelTrack( guid, "Transform", T::TransformChannel{} ) ) );
    EXPECT_TRUE( T::TrackHasKeys( ChannelTrack( guid, "Transform", KeyedTransform() ) ) );

    T::VectorChannel vec;
    vec.Z.Keys.push_back( Key( 0, 2.0F ) );
    EXPECT_TRUE( T::TrackHasKeys( ChannelTrack( guid, "Color", vec ) ) );

    T::EventChannel events;
    EXPECT_FALSE( T::TrackHasKeys( ChannelTrack( guid, "Events", events ) ) );
    events.Keys.push_back(
         T::EventKey{ Desert::Animation::FrameNumber{ 5 }, Desert::Animation::FrameNumber{ 0 }, "Fire", 0 } );
    EXPECT_TRUE( T::TrackHasKeys( ChannelTrack( guid, "Events", events ) ) );

    // An Animation section has no content channel: unkeyed, until its weight is keyed (UE's "Weight" channel).
    T::Track anim;
    anim.Binding  = guid;
    anim.Property = "Animation";
    anim.Kind     = T::TrackKind::Animation;
    T::Section section;
    section.Content = T::AnimationSectionContent{};
    anim.Sections.push_back( section );
    EXPECT_FALSE( T::TrackHasKeys( anim ) );
    anim.Sections[0].Weight.push_back( Key( 0, 0.5F ) );
    EXPECT_TRUE( T::TrackHasKeys( anim ) );

    T::Track cut;
    cut.Kind = T::TrackKind::CameraCut;
    T::Section cutSection;
    cutSection.Content = T::CameraCutSectionContent{};
    cut.Sections.push_back( cutSection );
    EXPECT_FALSE( T::TrackHasKeys( cut ) );
}

TEST( SequencerTrackFilter, SelectedIsPerActorKeyedIsPerTrackBothAreAnd )
{
    T::Sequence      sequence;
    const T::Binding keyedActor = Actor( "keyed" );
    const T::Binding emptyActor = Actor( "empty" );
    sequence.Bindings           = { keyedActor, emptyActor };
    sequence.Tracks.push_back( ChannelTrack( keyedActor.Guid, "Transform", KeyedTransform() ) );
    sequence.Tracks.push_back( ChannelTrack( keyedActor.Guid, "Visible", T::BoolChannel{} ) );
    sequence.Tracks.push_back( ChannelTrack( emptyActor.Guid, "Transform", T::TransformChannel{} ) );
    const T::Track& keyedTrack   = sequence.Tracks[0];
    const T::Track& unkeyedTrack = sequence.Tracks[1];

    const TrackFilters none{};
    const TrackFilters selected{ .Selected = true };
    const TrackFilters keyed{ .Keyed = true };
    const TrackFilters both{ .Selected = true, .Keyed = true };

    // Off: everything, selected or not.
    EXPECT_TRUE( BindingPasses( sequence, emptyActor, false, none ) );
    EXPECT_TRUE( TrackPasses( unkeyedTrack, false, none ) );

    // Selected: the actor decides, for every track under it.
    EXPECT_TRUE( BindingPasses( sequence, emptyActor, true, selected ) );
    EXPECT_FALSE( BindingPasses( sequence, keyedActor, false, selected ) );
    EXPECT_TRUE( TrackPasses( unkeyedTrack, true, selected ) );
    EXPECT_FALSE( TrackPasses( keyedTrack, false, selected ) );

    // Keyed: the track decides; the actor shows when one of ITS tracks does.
    EXPECT_TRUE( BindingPasses( sequence, keyedActor, false, keyed ) );
    EXPECT_FALSE( BindingPasses( sequence, emptyActor, false, keyed ) );
    EXPECT_TRUE( TrackPasses( keyedTrack, false, keyed ) );
    EXPECT_FALSE( TrackPasses( unkeyedTrack, false, keyed ) );

    // Both: AND.
    EXPECT_TRUE( BindingPasses( sequence, keyedActor, true, both ) );
    EXPECT_FALSE( BindingPasses( sequence, keyedActor, false, both ) );
    EXPECT_FALSE( BindingPasses( sequence, emptyActor, true, both ) );
    EXPECT_FALSE( TrackPasses( unkeyedTrack, true, both ) );
}
