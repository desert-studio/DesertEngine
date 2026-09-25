#include "AnimationClipBuild.hpp"

#include <algorithm>
#include <unordered_set>

namespace Desert::Assets::Serialization
{
    Common::ResultStr<Animation::AnimationClip> BuildClipFromAssetData( const AnimationAssetData& data )
    {
        // The generation is not checked here: it lives in the file's header, and ReadAnimationJson refuses
        // any file that does not state this build's (a v0 file's float seconds read as ticks would put every
        // key on tick 0). Every caller of this function holds data that came through it or was built here.
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
            {
                Animation::PositionKeyFrame key;
                key.Tick          = Animation::FrameNumber{ p.Tick };
                key.Position      = p.Value;
                key.Interp        = static_cast<Animation::KeyInterp>( p.Shape.Interp );
                key.Mode          = static_cast<Animation::TangentMode>( p.Shape.Mode );
                key.ArriveTangent = p.ArriveTangent;
                key.LeaveTangent  = p.LeaveTangent;
                track.PositionKeys.push_back( key );
            }

            track.RotationKeys.reserve( channel.Rotations.size() );
            for ( const auto& r : channel.Rotations )
            {
                const auto interp = static_cast<Animation::KeyInterp>( r.Shape.Interp );
                if ( interp == Animation::KeyInterp::Cubic )
                {
                    // REFUSED WHERE THE CLIP IS BUILT, so it cannot reach the sampler and be quietly
                    // treated as linear. A cubic through quaternions leaves the unit sphere; the curve
                    // that does not is `squad`, which builds its own control quaternions and is a
                    // different feature with a different authoring surface.
                    return Common::MakeFormattedError<Animation::AnimationClip>(
                         "clip '{}': the rotation channel of bone '{}' states Cubic interpolation at tick "
                         "{}. A cubic through quaternions is not a rotation — only Constant and Linear are "
                         "meaningful here until squad exists.",
                         data.Name, channel.BoneName, r.Tick );
                }
                Animation::RotationKeyFrame key;
                key.Tick     = Animation::FrameNumber{ r.Tick };
                key.Rotation = r.Value;
                key.Interp   = interp;
                track.RotationKeys.push_back( key );
            }

            track.ScaleKeys.reserve( channel.Scales.size() );
            for ( const auto& s : channel.Scales )
            {
                // THE SHAPE AND BOTH TANGENTS ARE READ, and until A28 they were not — the scale branch
                // built `{ Tick, Value }` and stopped, while `AnimationClipWrite` wrote all three. A
                // scale channel authored as Cubic with hand-set tangents therefore came back Linear/Auto
                // with flat slopes, and the file still held the numbers that said otherwise: a save, a
                // load and a second save silently rewrote the animator's curve. Both ends of the chain
                // looked right; the middle link dropped a property.
                Animation::ScaleKeyFrame key;
                key.Tick          = Animation::FrameNumber{ s.Tick };
                key.Scale         = s.Value;
                key.Interp        = static_cast<Animation::KeyInterp>( s.Shape.Interp );
                key.Mode          = static_cast<Animation::TangentMode>( s.Shape.Mode );
                key.ArriveTangent = s.ArriveTangent;
                key.LeaveTangent  = s.LeaveTangent;
                track.ScaleKeys.push_back( key );
            }

            clip.Tracks.push_back( std::move( track ) );
        }

        // ---- sections (generation 3) -----------------------------------------------------------------
        //
        // REFUSED RATHER THAN REPAIRED when a section makes no sense, for the reason every refusal in this
        // function exists: a clip that loads with a section nothing can evaluate animates wrongly and
        // silently, and "the character moved oddly" is the most expensive kind of bug report.
        clip.Sections.reserve( data.Sections.size() );
        for ( size_t i = 0; i < data.Sections.size(); ++i )
        {
            const SectionData& section = data.Sections[i];
            if ( section.EndTick < section.StartTick )
            {
                return Common::MakeFormattedError<Animation::AnimationClip>(
                     "clip '{}': section {} ('{}') runs from tick {} to {}, which is backwards. A section "
                     "covers no tick at all then, and the tracks it speaks for would silently play "
                     "unsectioned.",
                     data.Name, i, section.Name, section.StartTick, section.EndTick );
            }
            if ( section.Blend < 0 ||
                 section.Blend > static_cast<int32_t>( Animation::SectionBlendType::Additive ) )
            {
                return Common::MakeFormattedError<Animation::AnimationClip>(
                     "clip '{}': section {} ('{}') states blend type {}, which this build does not have. "
                     "Reading it as Absolute would turn an offset into a pose, which is wrong by the whole "
                     "rest pose rather than by a little.",
                     data.Name, i, section.Name, section.Blend );
            }

            Animation::ClipSection built;
            built.Name   = section.Name;
            built.Start  = Animation::FrameNumber{ section.StartTick };
            built.End    = Animation::FrameNumber{ section.EndTick };
            built.Blend  = static_cast<Animation::SectionBlendType>( section.Blend );
            built.Tracks = section.Tracks;
            built.Weight.reserve( section.Weight.size() );
            for ( const auto& w : section.Weight )
            {
                Animation::ScalarKey key;
                key.Tick          = Animation::FrameNumber{ w.Tick };
                key.Value         = w.Value;
                key.Interp        = static_cast<Animation::KeyInterp>( w.Shape.Interp );
                key.Mode          = static_cast<Animation::TangentMode>( w.Shape.Mode );
                key.ArriveTangent = w.ArriveTangent;
                key.LeaveTangent  = w.LeaveTangent;
                built.Weight.push_back( key );
            }
            clip.Sections.push_back( std::move( built ) );
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
