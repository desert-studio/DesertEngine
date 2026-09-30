#include "ClipMigration.hpp"
#include "ClipInterpShift.hpp"

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Common/Content/CanonicalText.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
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
                BoneTransform expected = clip.SampleTrack( clip.Tracks[b], at, reference );
                // THE ONE STATED DIVERGENCE: generation 3's rotation read the PREVIOUS key on the tick of a
                // Constant key; the sequence reads that key there (a key's tick reads the key, I8b-4b) —
                // asserted to be the key's own value, not skipped.
                for ( std::size_t k = 1; k < clip.Tracks[b].RotationKeys.size(); ++k )
                {
                    const auto& key = clip.Tracks[b].RotationKeys[k];
                    if ( key.Tick.Value == tick && key.Interp == Animation::KeyInterp::Constant )
                        expected.Rotation = key.Rotation;
                }
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
                const float expected = curve->Evaluate( at, clip.TickRate );
                if ( got != expected )
                    return Common::MakeFormattedError<bool>( "curve '{}' differs at tick {} ({} != {})",
                                                             track.Property, tick, got, expected );
            }
        }
        return Common::MakeSuccess( true );
    }

    namespace
    {
        // The clip around a sequence this tool read itself (a TMLN v1 block, which the engine's reader
        // refuses), written by the one writer: the GUID and import record stay, the writer restamps ANIM/TMLN.
        Common::ResultStr<std::string> WriteClip( const Assets::Serialization::AnimationAssetData& source,
                                                  Animation::Timeline::Sequence                    sequence )
        {
            using namespace Assets::Serialization;
            if ( sequence.Host != Animation::Timeline::SequenceHost::AnimationClip )
                return Common::MakeFormattedError<std::string>( "clip '{}': its timeline block is a {} sequence",
                                                                source.Name,
                                                                Animation::Timeline::ToString( sequence.Host ) );
            Animation::AnimationClip clip;
            clip.AnimationName     = source.Name;
            clip.SkeletonSignature = source.SkeletonSignature;
            clip.Sequence          = std::move( sequence );
            auto data              = BuildAssetDataFromClip( clip );
            if ( !data )
                return Common::MakeFormattedError<std::string>( "{}", data.GetError() );
            AnimationAssetData out = data.ExtractValue();
            out.Header             = source.Header;
            out.Import             = source.Import;
            return Common::Content::CanonicalJsonTextOfWriterOutput( WriteAnimationJson( out ) );
        }
    } // namespace

    Common::ResultStr<InterpShiftOutcome> MigrateClipInterpShift( const std::string& text )
    {
        using namespace Assets::Serialization;
        // The strict reader refuses a v5 header, and must: the body parses with the current layout (v6
        // changed what a key's mode MEANS, not where it is written). Its block states TMLN v1.
        auto parsed = Common::Json::Read<AnimationAssetData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<InterpShiftOutcome>( "not an ANIM v5 clip: {}", parsed.GetError() );
        const AnimationAssetData& source = parsed.GetValue();
        auto                      shifted = ShiftTimelineV1( Common::Json::Write( source.Sequence ) );
        if ( !shifted )
            return Common::MakeFormattedError<InterpShiftOutcome>( "clip '{}': {}", source.Name,
                                                                   shifted.GetError() );
        TimelineShift      shift = shifted.ExtractValue();
        InterpShiftOutcome outcome;
        outcome.KeyLists      = shift.KeyLists;
        outcome.SamplesProved = shift.SamplesProved;
        auto written          = WriteClip( source, std::move( shift.Shifted ) );
        if ( !written )
            return Common::MakeFormattedError<InterpShiftOutcome>( "{}", written.GetError() );
        outcome.Text = written.ExtractValue();
        return Common::MakeSuccess( std::move( outcome ) );
    }

    Common::ResultStr<std::optional<std::string>> MigrateClipTimelineV1ToV2( const std::string& text )
    {
        using namespace Assets::Serialization;
        auto parsed = Common::Json::Read<AnimationAssetData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<std::optional<std::string>>( "not an ANIM v6 clip: {}",
                                                                           parsed.GetError() );
        const AnimationAssetData& source = parsed.GetValue();
        const std::string         block  = Common::Json::Write( source.Sequence );
        const auto                stated = StatedTimelineVersion( block );
        if ( !stated )
            return Common::MakeFormattedError<std::optional<std::string>>( "clip '{}': {}", source.Name,
                                                                           stated.GetError() );
        if ( stated.GetValue() != Animation::Timeline::kTimelineLastArrivingInterpVersion )
            return Common::MakeSuccess( std::optional<std::string>{} );
        // ANIM v6 already shifted the modes (MigrateClipInterpShift, LiftClip): the number alone moves.
        auto sequence = ReadTimelineV1( block );
        if ( !sequence )
            return Common::MakeFormattedError<std::optional<std::string>>( "clip '{}': {}", source.Name,
                                                                           sequence.GetError() );
        auto written = WriteClip( source, sequence.ExtractValue() );
        if ( !written )
            return Common::MakeFormattedError<std::optional<std::string>>( "{}", written.GetError() );
        return Common::MakeSuccess( std::optional<std::string>( written.ExtractValue() ) );
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
