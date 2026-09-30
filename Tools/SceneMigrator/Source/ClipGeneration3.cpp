// Generation-3 clip behaviour and builder, moved verbatim from the engine (see ClipGeneration3.hpp).
#include "ClipGeneration3.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace Desert::Migration::ClipGen3
{
    using namespace Desert::Animation;

    namespace
    {
        bool SameState( const AnimationNotify& a, const AnimationNotify& b )
        {
            // Track is left out on purpose: moving a state to another row is not a new state.
            return a.Name == b.Name && a.Tick == b.Tick && a.DurationTicks == b.DurationTicks;
        }

        bool Contains( const std::vector<AnimationNotify>& list, const AnimationNotify& notify )
        {
            return std::any_of( list.begin(), list.end(),
                                [&notify]( const AnimationNotify& other ) { return SameState( other, notify ); } );
        }
    } // namespace

    void StepNotifyStates( const std::vector<AnimationNotify>& notifies, std::vector<AnimationNotify>& active,
                           const double before, const double after, const bool forwardPlayback, const bool looped,
                           std::vector<NotifyEvent>& out )
    {
        std::vector<AnimationNotify> now;
        for ( const auto& notify : notifies )
        {
            if ( NotifyStateActiveAt( notify, after ) )
            {
                now.push_back( notify );
            }
        }

        for ( const auto& was : active )
        {
            if ( !Contains( now, was ) )
            {
                out.push_back( NotifyEvent{ was.Name, NotifyEventKind::End } );
            }
        }

        for ( const auto& notify : notifies )
        {
            const bool crossed = forwardPlayback &&
                                 NotifyCrossed( static_cast<double>( notify.Tick.Value ), before, after, looped );
            if ( !notify.IsState() )
            {
                if ( crossed )
                {
                    out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::Fire } );
                }
                continue;
            }
            const bool isActive  = Contains( now, notify );
            const bool wasActive = Contains( active, notify );
            if ( isActive && !wasActive )
            {
                out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::Begin } );
            }
            else if ( !isActive && !wasActive && crossed )
            {
                // Entered and left inside one step (a state shorter than the frame): a script still hears
                // both halves, in order, rather than neither.
                out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::Begin } );
                out.push_back( NotifyEvent{ notify.Name, NotifyEventKind::End } );
            }
        }

        active = std::move( now );
    }

    float AnimationCurve::Evaluate( const FrameTime at, const FrameRate tickRate ) const
    {
        if ( Keys.empty() )
        {
            return 0.0F;
        }
        const double t = at.AsTicks();
        // The first key strictly AFTER the sample: a sample exactly on a key, or a sub-tick past it, is
        // then inside the segment that key starts — never a factor above 1 in the one before it.
        const auto next = std::upper_bound( Keys.begin(), Keys.end(), t, []( double tick, const ScalarKey& key )
                                            { return tick < static_cast<double>( key.Tick.Value ); } );
        if ( next == Keys.begin() )
        {
            return Keys.front().Value;
        }
        if ( next == Keys.end() )
        {
            return Keys.back().Value;
        }
        const auto   prev   = next - 1;
        const auto   span   = static_cast<double>( next->Tick.Value - prev->Tick.Value );
        const auto   factor = static_cast<float>( ( t - static_cast<double>( prev->Tick.Value ) ) / span );
        const double spanSeconds =
             span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
        return EvaluateSegment( prev->Value, prev->LeaveTangent, next->Value, next->ArriveTangent, next->Interp,
                                spanSeconds, factor );
    }

    float ClipSection::WeightAt( FrameTime at, FrameRate tickRate ) const
    {
        if ( Weight.empty() )
        {
            return 1.0F; // no fade authored — see the field note, this is not the same as a key of 0
        }
        if ( Weight.size() == 1 )
        {
            return Weight[0].Value;
        }

        // THE SAME BRACKETING A VEC3 CHANNEL DOES, and deliberately the same shape: integer comparison to
        // find the pair, a float only for the fraction inside one interval. A weight channel that
        // interpolated differently from the channels it scales would make a fade look like a defect in
        // the curve underneath it.
        const auto it =
             std::lower_bound( Weight.begin(), Weight.end(), at.Frame,
                               []( const ScalarKey& key, FrameNumber tick ) { return key.Tick < tick; } );
        if ( it == Weight.begin() )
        {
            return Weight.front().Value;
        }
        if ( it == Weight.end() )
        {
            return Weight.back().Value;
        }

        const auto prev = it - 1;
        const auto next = it;
        const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
        if ( span <= 0.0 )
        {
            return next->Value; // two keys on one tick: the later one is what a sampler there means
        }
        const auto   factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );
        const double spanSeconds =
             span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
        return EvaluateSegment( prev->Value, prev->LeaveTangent, next->Value, next->ArriveTangent, next->Interp,
                                spanSeconds, factor );
    }

    BoneTransform ApplySection( const ClipSection& section, const BoneTransform& authored,
                                const BoneTransform& reference, float weight )
    {
        // THE TWO ENDPOINTS ARE EXACT, AND THAT IS THE WHOLE POINT OF THE BRANCHES. See the header: the
        // corpus is entirely full-weight Absolute sections, so weight 1 has to be `authored` itself and
        // not an arithmetic result that rounds to it.
        if ( section.Blend == SectionBlendType::Absolute )
        {
            if ( weight >= 1.0F )
            {
                return authored;
            }
            if ( weight <= 0.0F )
            {
                return reference;
            }
            BoneTransform out;
            out.Translation = reference.Translation + weight * ( authored.Translation - reference.Translation );
            out.Rotation    = glm::slerp( reference.Rotation, authored.Rotation, weight );
            out.Scale       = reference.Scale + weight * ( authored.Scale - reference.Scale );
            return out;
        }

        // ADDITIVE: the track holds an OFFSET, so the identity value is a no-op at every weight and the
        // reference is what the offset is applied TO rather than what it is measured from. Scaling an
        // offset is what makes "a 50 % layer" mean half the authored displacement, which is the sentence
        // the header commits the documentation to.
        if ( weight == 0.0F )
        {
            return reference;
        }
        BoneTransform out;
        out.Translation = reference.Translation + weight * authored.Translation;
        // A partial rotation offset is the authored one slerped from identity, then composed — NOT the
        // authored quaternion scaled, which is not a rotation. Composed on the LEFT of the reference for
        // the same reason an additive layer is: the offset is expressed in the reference's own space.
        const glm::quat identity( 1.0F, 0.0F, 0.0F, 0.0F );
        const glm::quat partial =
             ( weight >= 1.0F ) ? authored.Rotation : glm::slerp( identity, authored.Rotation, weight );
        out.Rotation = reference.Rotation * partial;
        // Scale is MULTIPLICATIVE, so its identity is 1 and a partial offset walks from 1 towards the
        // authored factor. Adding it instead would make an unweighted additive scale of 1 double the bone.
        out.Scale = reference.Scale * ( glm::vec3( 1.0F ) + weight * ( authored.Scale - glm::vec3( 1.0F ) ) );
        return out;
    }

    namespace
    {
        /// One sentence, one spelling. The range rule is checked by four entry points and a second copy
        /// of it is a fifth rule that disagrees with the other four on the next change.
        Common::BoolResultStr CheckRange( FrameNumber start, FrameNumber end, FrameNumber duration )
        {
            if ( duration.Value < 0 )
            {
                return Common::MakeFormattedError<bool>( "a clip cannot be {} ticks long", duration.Value );
            }
            if ( end < start )
            {
                return Common::MakeFormattedError<bool>( "a section ends before it starts: [{}, {}]", start.Value,
                                                         end.Value );
            }
            if ( start.Value < 0 || duration < end )
            {
                return Common::MakeFormattedError<bool>( "[{}, {}] leaves the clip, which is [0, {}]", start.Value,
                                                         end.Value, duration.Value );
            }
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr CheckIndex( const std::vector<ClipSection>& sections, size_t index )
        {
            if ( index >= sections.size() )
            {
                return Common::MakeFormattedError<bool>( "section {} of {}: there is no such section", index,
                                                         sections.size() );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::BoolResultStr AddSection( std::vector<ClipSection>& sections, std::string name, FrameNumber start,
                                      FrameNumber end, SectionBlendType blend, FrameNumber duration )
    {
        if ( name.empty() )
        {
            // NOT auto-numbered here. A name is what the animator reads in the lane, and a function that
            // invented one would be a second naming scheme beside the panel's — which is how the lane and
            // the inspector come to show different names for one section.
            return Common::MakeError<bool>( "a section needs a name" );
        }
        if ( const auto range = CheckRange( start, end, duration ); !range.IsSuccess() )
        {
            return range;
        }

        ClipSection section;
        section.Name  = std::move( name );
        section.Start = start;
        section.End   = end;
        section.Blend = blend;
        // Tracks EMPTY and Weight EMPTY: every track, at full weight. The identity of the blend, which is
        // what a section the animator has not narrowed yet MEANS -- not "nothing" and not "silence".
        sections.push_back( std::move( section ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr MoveSection( std::vector<ClipSection>& sections, size_t index, int32_t deltaTicks,
                                       FrameNumber duration )
    {
        if ( const auto valid = CheckIndex( sections, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        ClipSection&      section = sections[index];
        const FrameNumber start{ section.Start.Value + deltaTicks };
        const FrameNumber end{ section.End.Value + deltaTicks };
        // REFUSES RATHER THAN CLAMPS, and the header says why: clamping the leading end alone is a move
        // that silently becomes a resize the moment the section touches tick 0 or the clip's last tick.
        if ( const auto range = CheckRange( start, end, duration ); !range.IsSuccess() )
        {
            return range;
        }
        section.Start = start;
        section.End   = end;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetSectionRange( std::vector<ClipSection>& sections, size_t index, FrameNumber start,
                                           FrameNumber end, FrameNumber duration )
    {
        if ( const auto valid = CheckIndex( sections, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        if ( const auto range = CheckRange( start, end, duration ); !range.IsSuccess() )
        {
            return range;
        }
        sections[index].Start = start;
        sections[index].End   = end;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveSection( std::vector<ClipSection>& sections, size_t index )
    {
        if ( const auto valid = CheckIndex( sections, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        sections.erase( sections.begin() + static_cast<std::ptrdiff_t>( index ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ReorderSection( std::vector<ClipSection>& sections, size_t index, int delta )
    {
        if ( const auto valid = CheckIndex( sections, index ); !valid.IsSuccess() )
        {
            return valid;
        }
        if ( delta != -1 && delta != 1 )
        {
            return Common::MakeFormattedError<bool>( "a section moves one place at a time, not {}", delta );
        }
        const auto target = static_cast<std::ptrdiff_t>( index ) + delta;
        if ( target < 0 || target >= static_cast<std::ptrdiff_t>( sections.size() ) )
        {
            return Common::MakeFormattedError<bool>( "section {} is already at the {} of the list", index,
                                                     delta < 0 ? "start" : "end" );
        }
        std::swap( sections[index], sections[static_cast<size_t>( target )] );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetSectionSpeaksFor( ClipSection& section, const std::string& track, bool on,
                                               const std::vector<std::string>& allTracks )
    {
        if ( std::find( allTracks.begin(), allTracks.end(), track ) == allTracks.end() )
        {
            return Common::MakeFormattedError<bool>( "the clip has no track called '{}'", track );
        }

        // THE EMPTY LIST IS EXPANDED BEFORE THE EDIT, NOT AFTER IT. Unticking one box on a clip-wide
        // section means "all of them except this one", and that sentence cannot be written as a removal
        // from an empty list -- the naive version leaves the list empty and the box ticked again next frame.
        std::vector<std::string> named = section.Tracks.empty() ? allTracks : section.Tracks;
        const auto               found = std::find( named.begin(), named.end(), track );
        if ( on )
        {
            if ( found == named.end() )
            {
                named.push_back( track );
            }
        }
        else if ( found != named.end() )
        {
            named.erase( found );
        }

        if ( named.empty() )
        {
            // Every box unticked. NOT the same as `Tracks` empty, which is every box TICKED -- so this is
            // refused rather than written: a section that speaks for nothing is indistinguishable on disk
            // from one that speaks for everything, and it is the second one every reader would assume.
            return Common::MakeError<bool>(
                 "a section must speak for at least one track — delete it instead of emptying it" );
        }
        if ( named.size() == allTracks.size() )
        {
            // Every track named. One meaning, one spelling: see the header.
            section.Tracks.clear();
            return Common::MakeSuccess( true );
        }
        section.Tracks = std::move( named );
        return Common::MakeSuccess( true );
    }

    void SetSectionSpeaksForEveryTrack( ClipSection& section )
    {
        section.Tracks.clear();
    }

    Common::BoolResultStr SetSectionWeightKey( ClipSection& section, FrameNumber tick, float value )
    {
        if ( !std::isfinite( value ) )
        {
            return Common::MakeError<bool>( "a section weight has to be a number" );
        }
        if ( tick.Value < 0 )
        {
            return Common::MakeFormattedError<bool>( "tick {} is before the clip starts", tick.Value );
        }
        const float clamped = std::clamp( value, 0.0F, 1.0F );

        const auto at =
             std::lower_bound( section.Weight.begin(), section.Weight.end(), tick,
                               []( const ScalarKey& key, FrameNumber want ) { return key.Tick < want; } );
        if ( at != section.Weight.end() && !( tick < at->Tick ) )
        {
            // AN EXISTING KEY KEEPS ITS SHAPE, exactly as `TrackEditing::SetTransformKey` does: changing a
            // value is not permission to discard the slope somebody authored.
            at->Value = clamped;
            return Common::MakeSuccess( true );
        }

        ScalarKey key;
        key.Tick   = tick;
        key.Value  = clamped;
        key.Interp = KeyInterp::Linear; // a fade is a ramp; see the header for why not Cubic
        key.Mode   = TangentMode::Auto;
        section.Weight.insert( at, key );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveSectionWeightKey( ClipSection& section, size_t keyIndex )
    {
        if ( keyIndex >= section.Weight.size() )
        {
            return Common::MakeFormattedError<bool>( "weight key {} of {}: there is no such key", keyIndex,
                                                     section.Weight.size() );
        }
        section.Weight.erase( section.Weight.begin() + static_cast<std::ptrdiff_t>( keyIndex ) );
        return Common::MakeSuccess( true );
    }

    void ClearSectionWeight( ClipSection& section )
    {
        section.Weight.clear();
    }

    Common::ResultStr<AnimationClip> BuildClip( const AnimationAssetData& data )
    {
        // The generation is not checked here: it lives in the file's header, and ReadAnimationJson refuses
        // any file that does not state this build's (a v0 file's float seconds read as ticks would put every
        // key on tick 0). Every caller of this function holds data that came through it or was built here.
        const FrameRate tickRate{ data.TickRate.Numerator, data.TickRate.Denominator };
        if ( !tickRate.IsValid() )
        {
            return Common::MakeFormattedError<AnimationClip>(
                 "clip '{}' states a tick rate of {}/{}. A rate of zero is not a slow clock, it is a "
                 "missing one: every key time in the file would be uninterpretable.",
                 data.Name, data.TickRate.Numerator, data.TickRate.Denominator );
        }
        const FrameRate displayRate{ data.DisplayRate.Numerator, data.DisplayRate.Denominator };
        if ( !displayRate.IsValid() )
        {
            return Common::MakeFormattedError<AnimationClip>(
                 "clip '{}' states a display rate of {}/{}, which is not a grid anything can be shown on.",
                 data.Name, data.DisplayRate.Numerator, data.DisplayRate.Denominator );
        }

        AnimationClip clip;
        clip.AnimationName     = data.Name;
        clip.DurationTicks     = FrameNumber{ data.DurationTicks };
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
                return Common::MakeFormattedError<AnimationClip>(
                     "clip '{}': channel {} of {} names no bone. The bone name is the only key playback binds "
                     "on, so these {} position / {} rotation / {} scale keys could never reach a skeleton.",
                     data.Name, i, data.Channels.size(), channel.Positions.size(), channel.Rotations.size(),
                     channel.Scales.size() );
            }

            if ( !claimed.insert( channel.BoneName ).second )
            {
                return Common::MakeFormattedError<AnimationClip>(
                     "clip '{}': channel {} claims bone '{}', which an earlier channel already claims. "
                     "Playback resolves a bone to ONE track, so one of the two would be dropped without a "
                     "word.",
                     data.Name, i, channel.BoneName );
            }

            BoneTrack track;
            track.BoneName = channel.BoneName;

            track.PositionKeys.reserve( channel.Positions.size() );
            for ( const auto& p : channel.Positions )
            {
                PositionKeyFrame key;
                key.Tick          = FrameNumber{ p.Tick };
                key.Position      = p.Value;
                key.Interp        = static_cast<KeyInterp>( p.Shape.Interp );
                key.Mode          = static_cast<TangentMode>( p.Shape.Mode );
                key.ArriveTangent = p.ArriveTangent;
                key.LeaveTangent  = p.LeaveTangent;
                track.PositionKeys.push_back( key );
            }

            track.RotationKeys.reserve( channel.Rotations.size() );
            for ( const auto& r : channel.Rotations )
            {
                const auto interp = static_cast<KeyInterp>( r.Shape.Interp );
                if ( interp == KeyInterp::Cubic )
                {
                    // REFUSED WHERE THE CLIP IS BUILT, so it cannot reach the sampler and be quietly
                    // treated as linear. A cubic through quaternions leaves the unit sphere; the curve
                    // that does not is `squad`, which builds its own control quaternions and is a
                    // different feature with a different authoring surface.
                    return Common::MakeFormattedError<AnimationClip>(
                         "clip '{}': the rotation channel of bone '{}' states Cubic interpolation at tick "
                         "{}. A cubic through quaternions is not a rotation — only Constant and Linear are "
                         "meaningful here until squad exists.",
                         data.Name, channel.BoneName, r.Tick );
                }
                RotationKeyFrame key;
                key.Tick     = FrameNumber{ r.Tick };
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
                ScaleKeyFrame key;
                key.Tick          = FrameNumber{ s.Tick };
                key.Scale         = s.Value;
                key.Interp        = static_cast<KeyInterp>( s.Shape.Interp );
                key.Mode          = static_cast<TangentMode>( s.Shape.Mode );
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
                return Common::MakeFormattedError<AnimationClip>(
                     "clip '{}': section {} ('{}') runs from tick {} to {}, which is backwards. A section "
                     "covers no tick at all then, and the tracks it speaks for would silently play "
                     "unsectioned.",
                     data.Name, i, section.Name, section.StartTick, section.EndTick );
            }
            if ( section.Blend < 0 ||
                 section.Blend > static_cast<int32_t>( SectionBlendType::Additive ) )
            {
                return Common::MakeFormattedError<AnimationClip>(
                     "clip '{}': section {} ('{}') states blend type {}, which this build does not have. "
                     "Reading it as Absolute would turn an offset into a pose, which is wrong by the whole "
                     "rest pose rather than by a little.",
                     data.Name, i, section.Name, section.Blend );
            }

            ClipSection built;
            built.Name   = section.Name;
            built.Start  = FrameNumber{ section.StartTick };
            built.End    = FrameNumber{ section.EndTick };
            built.Blend  = static_cast<SectionBlendType>( section.Blend );
            built.Tracks = section.Tracks;
            built.Weight.reserve( section.Weight.size() );
            for ( const auto& w : section.Weight )
            {
                ScalarKey key;
                key.Tick          = FrameNumber{ w.Tick };
                key.Value         = w.Value;
                key.Interp        = static_cast<KeyInterp>( w.Shape.Interp );
                key.Mode          = static_cast<TangentMode>( w.Shape.Mode );
                key.ArriveTangent = w.ArriveTangent;
                key.LeaveTangent  = w.LeaveTangent;
                built.Weight.push_back( key );
            }
            clip.Sections.push_back( std::move( built ) );
        }

        // Notifies sorted by time so the Animator's crossing test is a simple ordered scan.
        clip.Notifies.reserve( data.Notifies.size() );
        for ( const auto& n : data.Notifies )
        {
            if ( n.Track < 0 )
            {
                return Common::MakeFormattedError<AnimationClip>(
                     "notify '{}' at tick {} states track {}; a notify track is a row index, 0 or more", n.Name,
                     n.Tick, n.Track );
            }
            if ( n.DurationTicks < 0 )
            {
                return Common::MakeFormattedError<AnimationClip>(
                     "notify '{}' at tick {} states duration {}; a notify state lasts 0 ticks (instant) or more",
                     n.Name, n.Tick, n.DurationTicks );
            }
            clip.Notifies.push_back( AnimationNotify{ n.Name, FrameNumber{ n.Tick }, n.Track,
                                                                 FrameNumber{ n.DurationTicks } } );
        }
        std::sort( clip.Notifies.begin(), clip.Notifies.end(),
                   []( const AnimationNotify& a, const AnimationNotify& b )
                   { return a.Tick < b.Tick; } );

        clip.Curves.reserve( data.Curves.size() );
        for ( const auto& curve : data.Curves )
        {
            if ( curve.Name.empty() )
            {
                return Common::MakeFormattedError<AnimationClip>(
                     "clip '{}' has an anim curve with no name ({} keys); a curve is read by its name", data.Name,
                     curve.Keys.size() );
            }
            if ( clip.FindCurve( curve.Name ) != nullptr )
            {
                return Common::MakeFormattedError<AnimationClip>(
                     "clip '{}' states anim curve '{}' twice; GetCurveValue could only ever read one of them",
                     data.Name, curve.Name );
            }
            AnimationCurve built;
            built.Name = curve.Name;
            built.Keys.reserve( curve.Keys.size() );
            for ( const auto& k : curve.Keys )
            {
                if ( k.Shape.Interp < 0 || k.Shape.Interp > static_cast<int>( KeyInterp::Cubic ) )
                {
                    return Common::MakeFormattedError<AnimationClip>(
                         "anim curve '{}' key at tick {} states interpolation {}; 0 constant, 1 linear, 2 cubic",
                         curve.Name, k.Tick, k.Shape.Interp );
                }
                ScalarKey key;
                key.Tick          = FrameNumber{ k.Tick };
                key.Value         = k.Value;
                key.Interp        = static_cast<KeyInterp>( k.Shape.Interp );
                key.Mode          = static_cast<TangentMode>( k.Shape.Mode );
                key.ArriveTangent = k.ArriveTangent;
                key.LeaveTangent  = k.LeaveTangent;
                built.Keys.push_back( key );
            }
            std::stable_sort( built.Keys.begin(), built.Keys.end(),
                              []( const ScalarKey& a, const ScalarKey& b )
                              { return a.Tick < b.Tick; } );
            clip.Curves.push_back( std::move( built ) );
        }

        return Common::MakeSuccess( std::move( clip ) );
    }
} // namespace Desert::Migration::ClipGen3
