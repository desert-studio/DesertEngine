#include "ClipMigration.hpp"

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Common/Content/CanonicalText.hpp>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace Desert::Migration
{
    Common::BoolResultStr VerifyLift( const ClipGen3::AnimationClip& clip, const Animation::Timeline::Sequence& lift )
    {
        using namespace Desert::Animation::Timeline;
        using Animation::BoneTransform;
        using Animation::FrameNumber;
        using Animation::FrameTime;

        std::vector<Animation::BoneInfo> bones( clip.Tracks.size() );
        for ( std::size_t i = 0; i < bones.size(); ++i )
            bones[i].Name = clip.Tracks[i].BoneName;
        const Animation::Skeleton skeleton( std::move( bones ) );
        const BoneBindingTable    table = BindBones( lift, skeleton );
        if ( table.Missing != 0 )
            return Common::MakeFormattedError<bool>( "{} bone bindings of the lift found no bone", table.Missing );

        Evaluator           evaluator( lift );
        EvaluatedFrame      frame;
        const BoneTransform reference; // the rest value the pose is seeded with, ApplySection's `reference`
        for ( int32_t tick = 0; tick <= clip.DurationTicks.Value; ++tick )
        {
            const FrameTime      at{ FrameNumber{ tick }, 0.0F };
            Animation::LocalPose pose( clip.Tracks.size() );
            if ( auto evaluated = EvaluatePose( lift, table, at, pose ); !evaluated.IsSuccess() )
                return Common::MakeFormattedError<bool>( "tick {}: {}", tick, evaluated.GetError() );
            for ( std::size_t b = 0; b < clip.Tracks.size(); ++b )
            {
                const BoneTransform expected = clip.SampleTrack( clip.Tracks[b], at, reference );
                if ( pose[b].Translation != expected.Translation || pose[b].Rotation != expected.Rotation ||
                     pose[b].Scale != expected.Scale )
                    return Common::MakeFormattedError<bool>( "bone '{}' differs at tick {}", clip.Tracks[b].BoneName,
                                                             tick );
            }
            evaluator.Evaluate( TimeStep{ at, at }, frame );
            for ( const EvaluatedTrack& value : frame.Values )
            {
                const Track& track = lift.Tracks[value.TrackIndex];
                if ( track.Kind != TrackKind::Float )
                    continue;
                const ClipGen3::AnimationCurve* curve = clip.FindCurve( track.Property );
                if ( curve == nullptr )
                    return Common::MakeFormattedError<bool>( "curve '{}' has no source", track.Property );
                const float got = std::get<float>( value.Value );
                const auto  key = std::find_if( curve->Keys.begin(), curve->Keys.end(),
                                                [&]( const Animation::ScalarKey& k ) { return k.Tick.Value == tick; } );
                const bool constantKey = key != curve->Keys.begin() && key != curve->Keys.end() &&
                                         key->Interp == Animation::KeyInterp::Constant;
                const float expected = constantKey ? ( key - 1 )->Value : curve->Evaluate( at, clip.TickRate );
                if ( got != expected )
                    return Common::MakeFormattedError<bool>( "curve '{}' differs at tick {} ({} != {})",
                                                             track.Property, tick, got, expected );
            }
        }
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<ClipMigrationOutcome> MigrateClipGeneration3( const std::string& text )
    {
        auto parsed = Common::Json::Read<ClipGen3::AnimationAssetData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "not a generation-3 clip: {}", parsed.GetError() );
        const ClipGen3::AnimationAssetData& source = parsed.GetValue();
        if ( !source.Header )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "generation-3 clip states no header" );

        auto built = ClipGen3::BuildClip( source );
        if ( !built )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "{}", built.GetError() );
        const ClipGen3::AnimationClip& clip = built.GetValue();

        auto lifted = Animation::Timeline::LiftClip( clip );
        if ( !lifted )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "{}", lifted.GetError() );
        if ( auto proved = VerifyLift( clip, lifted.GetValue() ); !proved.IsSuccess() )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "clip '{}': the lift is not the identity: {}",
                                                                     clip.AnimationName, proved.GetError() );

        Animation::AnimationClip current;
        current.AnimationName     = clip.AnimationName;
        current.SkeletonSignature = clip.SkeletonSignature;
        current.Sequence          = lifted.ExtractValue();
        auto data = Assets::Serialization::BuildAssetDataFromClip( current );
        if ( !data )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "{}", data.GetError() );
        Assets::Serialization::AnimationAssetData out = data.ExtractValue();
        out.Header = source.Header; // the GUID stays the clip's identity; the writer restamps the versions
        out.Import = source.Import;
        auto canonical =
             Common::Content::CanonicalJsonTextOfWriterOutput( Assets::Serialization::WriteAnimationJson( out ) );
        if ( !canonical )
            return Common::MakeFormattedError<ClipMigrationOutcome>( "{}", canonical.GetError() );

        ClipMigrationOutcome outcome;
        outcome.Text        = canonical.ExtractValue();
        outcome.BoneTracks  = clip.Tracks.size();
        outcome.Curves      = clip.Curves.size();
        outcome.Notifies    = clip.Notifies.size();
        outcome.Sections    = clip.Sections.size();
        outcome.TicksProved = static_cast<std::size_t>( clip.DurationTicks.Value ) + 1;
        return Common::MakeSuccess( std::move( outcome ) );
    }
} // namespace Desert::Migration
