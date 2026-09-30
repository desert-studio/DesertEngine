#pragma once

// Shared by the TimelineContract suite's files: one fixture set, each file one group of the contract.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Graph/LayeredBlendPerBone.hpp>
#include <Engine/Animation/Graph/LinkedAnimLayer.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Animation/Timeline/Track.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <type_traits>

namespace TimelineFixtures
{
    using namespace Desert::Animation;
    using namespace Desert::Animation::Timeline;
    namespace G = Desert::Animation::Graph;
    using Common::Content::AssetGuid;

    inline FrameNumber Tick( int32_t value )
    {
        return FrameNumber{ value };
    }
    inline FrameTime At( int32_t tick, float subframe = 0.0F )
    {
        return FrameTime{ Tick( tick ), subframe };
    }
    inline ScalarKey Key( int32_t tick, float value, KeyInterp interp = KeyInterp::Linear )
    {
        ScalarKey key;
        key.Tick   = Tick( tick );
        key.Value  = value;
        key.Interp = interp;
        return key;
    }
    inline BindingGuid Guid( uint64_t lo )
    {
        return BindingGuid{ AssetGuid{ 1, lo } };
    }

    /// A LevelSequence with one entity and one Float track "Opacity" holding @p channel in one section.
    inline Sequence OneFloatTrack( FloatChannel channel )
    {
        Sequence sequence;
        sequence.Host  = SequenceHost::LevelSequence;
        sequence.Start = Tick( 0 );
        sequence.End   = Tick( 100 );
        sequence.Bindings.push_back( Binding{ Guid( 1 ), BindingKind::Entity, "entity-uuid", "Door", {} } );
        Track track;
        track.Binding  = Guid( 1 );
        track.Property = "Opacity";
        track.Kind     = TrackKind::Float;
        Section section;
        section.Start   = Tick( 0 );
        section.End     = Tick( 100 );
        section.Content = Channel{ std::move( channel ) };
        track.Sections.push_back( std::move( section ) );
        sequence.Tracks.push_back( std::move( track ) );
        return sequence;
    }
    } // namespace TimelineFixtures
