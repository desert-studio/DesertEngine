#include "AnimationClipBuild.hpp"

#include <algorithm>
#include <unordered_set>

namespace Desert::Assets::Serialization
{
    Common::ResultStr<Animation::AnimationClip> BuildClipFromAssetData( const AnimationAssetData& data )
    {
        // THE GENERATION IS CHECKED FIRST, AND A MISSING ONE IS 0 RATHER THAN "CURRENT". A `.anim` written
        // before this field existed reads back as 0 through DefaultIfMissing, and its keys carry `Time` in
        // float seconds under a `TicksPerSecond` of 1 — which, read as ticks, would be key 0 for every key
        // and a clip that animates nothing while loading cleanly. Refusing names the tool that converts it.
        if ( data.Version < kAnimationVersion )
        {
            return Common::MakeFormattedError<Animation::AnimationClip>(
                 "clip '{}' is at `.anim` generation {} and this build reads generation {}. Its key times "
                 "are float seconds, not ticks, so reading them here would put every key on tick 0 and "
                 "produce a clip that loads without a word and animates nothing. Convert it with "
                 "Tools/SceneMigrator, which collects `.anim` alongside the scene formats.",
                 data.Name, data.Version, kAnimationVersion );
        }
        if ( data.Version > kAnimationVersion )
        {
            return Common::MakeFormattedError<Animation::AnimationClip>(
                 "clip '{}' is at `.anim` generation {} and this build reads generation {}. A file from a "
                 "newer build may hold fields this one would drop on the next save.",
                 data.Name, data.Version, kAnimationVersion );
        }

        const Animation::FrameRate tickRate{ data.TickRate.Numerator, data.TickRate.Denominator };
        if ( !tickRate.IsValid() )
        {
            return Common::MakeFormattedError<Animation::AnimationClip>(
                 "clip '{}' states a tick rate of {}/{}. A rate of zero is not a slow clock, it is a "
                 "missing one: every key time in the file would be uninterpretable.",
                 data.Name, data.TickRate.Numerator, data.TickRate.Denominator );
        }
        const Animation::FrameRate displayRate{ data.DisplayRate.Numerator, data.DisplayRate.Denominator };
        if ( !displayRate.IsValid() )
        {
            return Common::MakeFormattedError<Animation::AnimationClip>(
                 "clip '{}' states a display rate of {}/{}, which is not a grid anything can be shown on.",
                 data.Name, data.DisplayRate.Numerator, data.DisplayRate.Denominator );
        }

        Animation::AnimationClip clip;
        clip.AnimationName     = data.Name;
        clip.DurationTicks     = Animation::FrameNumber{ data.DurationTicks };
        clip.TickRate          = tickRate;
        clip.DisplayRate       = displayRate;
        clip.SkeletonSignature = data.SkeletonSignature;
        clip.Tracks.reserve( data.Channels.size() );

        std::unordered_set<std::string> claimed;
        claimed.reserve( data.Channels.size() );

        for ( size_t i = 0; i < data.Channels.size(); ++i )
        {
            const auto& channel = data.Channels[i];

            if ( channel.BoneName.empty() )
            {
                return Common::MakeFormattedError<Animation::AnimationClip>(
                     "clip '{}': channel {} of {} names no bone. The bone name is the only key playback binds "
                     "on, so these {} position / {} rotation / {} scale keys could never reach a skeleton.",
                     data.Name, i, data.Channels.size(), channel.Positions.size(), channel.Rotations.size(),
                     channel.Scales.size() );
            }

            if ( !claimed.insert( channel.BoneName ).second )
            {
                return Common::MakeFormattedError<Animation::AnimationClip>(
                     "clip '{}': channel {} claims bone '{}', which an earlier channel already claims. "
                     "Playback resolves a bone to ONE track, so one of the two would be dropped without a "
                     "word.",
                     data.Name, i, channel.BoneName );
            }

            Animation::BoneTrack track;
            track.BoneName = channel.BoneName;

            track.PositionKeys.reserve( channel.Positions.size() );
            for ( const auto& p : channel.Positions )
                track.PositionKeys.push_back(
                     Animation::PositionKeyFrame{ Animation::FrameNumber{ p.Tick }, p.Value } );

            track.RotationKeys.reserve( channel.Rotations.size() );
            for ( const auto& r : channel.Rotations )
                track.RotationKeys.push_back(
                     Animation::RotationKeyFrame{ Animation::FrameNumber{ r.Tick }, r.Value } );

            track.ScaleKeys.reserve( channel.Scales.size() );
            for ( const auto& s : channel.Scales )
                track.ScaleKeys.push_back( Animation::ScaleKeyFrame{ Animation::FrameNumber{ s.Tick }, s.Value } );

            clip.Tracks.push_back( std::move( track ) );
        }

        // Notifies sorted by time so the Animator's crossing test is a simple ordered scan.
        clip.Notifies.reserve( data.Notifies.size() );
        for ( const auto& n : data.Notifies )
            clip.Notifies.push_back( Animation::AnimationNotify{ n.Name, Animation::FrameNumber{ n.Tick } } );
        std::sort( clip.Notifies.begin(), clip.Notifies.end(),
                   []( const Animation::AnimationNotify& a, const Animation::AnimationNotify& b )
                   { return a.Tick < b.Tick; } );

        return Common::MakeSuccess( std::move( clip ) );
    }
} // namespace Desert::Assets::Serialization
