#include "ClipSection.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <algorithm>

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
} // namespace Desert::Animation
