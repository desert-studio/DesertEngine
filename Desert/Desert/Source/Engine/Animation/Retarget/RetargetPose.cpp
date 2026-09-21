#include "RetargetPose.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <utility>

namespace Desert::Animation::Retarget
{
    void RetargetPose::SetBoneRotationOffset( std::string bone, const glm::quat& localDelta )
    {
        m_BoneRotationOffsets[std::move( bone )] = localDelta;
    }

    void RetargetPose::SetPelvisTranslationOffset( const glm::vec3& localOffset )
    {
        m_PelvisTranslationOffset = localOffset;
    }

    Common::ResultStr<LocalPose> RetargetPose::Apply( const Skeleton& skeleton, uint32_t pelvisBone ) const
    {
        auto bind = LocalPose::FromBindPose( skeleton );
        if ( !bind.IsSuccess() )
        {
            return Common::MakeFormattedError<LocalPose>( "a retarget pose needs the rig's bind pose: {}",
                                                          bind.GetError() );
        }
        LocalPose pose = bind.ExtractValue();

        for ( const auto& [name, delta] : m_BoneRotationOffsets )
        {
            const auto index = skeleton.FindBoneIndex( name );
            if ( !index.has_value() )
            {
                return Common::MakeFormattedError<LocalPose>(
                     "the retarget pose rotates a bone named '{}', and this rig of {} bones has no such "
                     "bone.",
                     name, skeleton.GetBones().size() );
            }
            pose[*index].Rotation = glm::normalize( pose[*index].Rotation * delta );
        }

        if ( pelvisBone >= pose.Size() )
        {
            return Common::MakeFormattedError<LocalPose>(
                 "the retarget pose's pelvis is bone {}, and this rig has {}.", pelvisBone, pose.Size() );
        }
        pose[pelvisBone].Translation += m_PelvisTranslationOffset;

        return Common::MakeSuccess( std::move( pose ) );
    }
} // namespace Desert::Animation::Retarget
