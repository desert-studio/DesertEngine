#include "ModelPose.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Desert::Animation::Retarget
{
    namespace
    {
        // A scale component below this has no invertible local frame. 1e-6 rather than exactly zero
        // because a rig authored in centimetres and scaled down twice reaches denormals long before it
        // reaches 0.0F, and dividing by 1e-30 produces an infinity that then reads as a bone at infinity
        // rather than as the refusal it is.
        constexpr float MIN_INVERTIBLE_SCALE = 1.0e-6F;
    } // namespace

    BoneTransform Compose( const BoneTransform& parent, const BoneTransform& child )
    {
        BoneTransform out;
        out.Rotation    = parent.Rotation * child.Rotation;
        out.Scale       = parent.Scale * child.Scale;
        out.Translation = parent.Translation + ( parent.Rotation * ( parent.Scale * child.Translation ) );
        return out;
    }

    Common::ResultStr<BoneTransform> Relative( const BoneTransform& parent, const BoneTransform& child )
    {
        for ( int axis = 0; axis < 3; ++axis )
        {
            if ( glm::abs( parent.Scale[axis] ) < MIN_INVERTIBLE_SCALE )
            {
                return Common::MakeFormattedError<BoneTransform>(
                     "a parent whose scale is ({:g}, {:g}, {:g}) has no local frame on axis {} to express a "
                     "child in.",
                     parent.Scale.x, parent.Scale.y, parent.Scale.z, axis );
            }
        }

        const glm::quat inverseRotation = glm::inverse( parent.Rotation );

        BoneTransform out;
        out.Rotation    = inverseRotation * child.Rotation;
        out.Scale       = child.Scale / parent.Scale;
        out.Translation = ( inverseRotation * ( child.Translation - parent.Translation ) ) / parent.Scale;
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<ModelPose> ModelPose::FromLocal( const Skeleton& skeleton, const LocalPose& local )
    {
        ModelPose out;
        out.m_Bones.assign( skeleton.GetBones().size(), BoneTransform{} );

        auto propagated = out.PropagateFrom( skeleton, local, 0 );
        if ( !propagated.IsSuccess() )
        {
            return Common::MakeError<ModelPose>( propagated.GetError() );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr ModelPose::PropagateFrom( const Skeleton& skeleton, const LocalPose& local,
                                                    uint32_t startRank )
    {
        const size_t boneCount = skeleton.GetBones().size();
        if ( local.Size() != boneCount )
        {
            return Common::MakeFormattedError<bool>(
                 "a pose of {} bones cannot be resolved through a skeleton of {}: the index space they "
                 "share is the whole invariant.",
                 local.Size(), boneCount );
        }
        m_Bones.resize( boneCount );

        const auto& order = skeleton.GetResolveOrder();
        for ( size_t rank = startRank; rank < order.size(); ++rank )
        {
            const uint32_t bone   = order[rank];
            const uint32_t parent = skeleton.ResolveParent( bone );

            m_Bones[bone] = parent == Skeleton::NO_PARENT ? local[bone] : Compose( m_Bones[parent], local[bone] );
        }
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<LocalPose> ModelPose::ToLocal( const Skeleton& skeleton ) const
    {
        const size_t boneCount = skeleton.GetBones().size();
        if ( m_Bones.size() != boneCount )
        {
            return Common::MakeFormattedError<LocalPose>(
                 "a model pose of {} bones does not belong to a skeleton of {}.", m_Bones.size(), boneCount );
        }

        LocalPose out( boneCount );
        for ( const uint32_t bone : skeleton.GetResolveOrder() )
        {
            const uint32_t parent = skeleton.ResolveParent( bone );
            if ( parent == Skeleton::NO_PARENT )
            {
                out[bone] = m_Bones[bone];
                continue;
            }

            auto relative = Relative( m_Bones[parent], m_Bones[bone] );
            if ( !relative.IsSuccess() )
            {
                return Common::MakeFormattedError<LocalPose>( "bone '{}' under '{}': {}",
                                                              skeleton.GetBones()[bone].Name,
                                                              skeleton.GetBones()[parent].Name,
                                                              relative.GetError() );
            }
            out[bone] = relative.ExtractValue();
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Animation::Retarget
