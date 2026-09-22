#include "ClipSection.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Desert::Animation
{
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
                return Common::MakeFormattedError<bool>(
                     "a section ends before it starts: [{}, {}]", start.Value, end.Value );
            }
            if ( start.Value < 0 || duration < end )
            {
                return Common::MakeFormattedError<bool>(
                     "[{}, {}] leaves the clip, which is [0, {}]", start.Value, end.Value, duration.Value );
            }
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr CheckIndex( const std::vector<ClipSection>& sections, size_t index )
        {
            if ( index >= sections.size() )
            {
                return Common::MakeFormattedError<bool>( "section {} of {}: there is no such section",
                                                         index, sections.size() );
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

        const auto at = std::lower_bound( section.Weight.begin(), section.Weight.end(), tick,
                                          []( const ScalarKey& key, FrameNumber want )
                                          { return key.Tick < want; } );
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
} // namespace Desert::Animation
