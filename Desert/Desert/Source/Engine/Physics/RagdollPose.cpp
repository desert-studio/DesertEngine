#include <Engine/Physics/RagdollPose.hpp>

#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <format>

namespace Desert::Physics
{
    namespace
    {
        constexpr float kUnitScaleTolerance = 1e-4f;

        glm::vec3 ColumnLengths( const glm::mat4& m )
        {
            return { glm::length( glm::vec3( m[0] ) ), glm::length( glm::vec3( m[1] ) ),
                     glm::length( glm::vec3( m[2] ) ) };
        }

        RagdollPartTransform SplitRigid( const glm::mat4& m )
        {
            const glm::vec3 scale = ColumnLengths( m );
            glm::mat3       basis( m );
            basis[0] /= scale.x;
            basis[1] /= scale.y;
            basis[2] /= scale.z;
            RagdollPartTransform t;
            t.Position = glm::vec3( m[3] );
            t.Rotation = glm::normalize( glm::quat_cast( basis ) );
            return t;
        }

        std::string CheckPose( const RagdollDesc& desc, const Animation::Skeleton& skeleton,
                               const Animation::LocalPose& pose )
        {
            const size_t bones = skeleton.GetBones().size();
            if ( pose.Size() != bones )
                return std::format( "the pose has {} bones, the skeleton {}", pose.Size(), bones );
            for ( const RagdollPartDesc& part : desc.Parts )
                if ( part.SkeletonBone >= bones )
                    return std::format( "ragdoll part '{}' names bone {}, the skeleton has {}", part.Bone,
                                        part.SkeletonBone, bones );
            return {};
        }
    } // namespace

    Common::ResultStr<std::vector<RagdollPartTransform>> RagdollPartsFromPose( const RagdollDesc&         desc,
                                                                               const Animation::Skeleton& skeleton,
                                                                               const Animation::LocalPose& pose,
                                                                               const glm::mat4& entityWorld )
    {
        using Result = std::vector<RagdollPartTransform>;
        if ( const std::string fault = CheckPose( desc, skeleton, pose ); !fault.empty() )
            return Common::MakeError<Result>( fault );
        const glm::vec3 entityScale = ColumnLengths( entityWorld );
        if ( std::abs( entityScale.x - 1.0f ) > kUnitScaleTolerance ||
             std::abs( entityScale.y - 1.0f ) > kUnitScaleTolerance ||
             std::abs( entityScale.z - 1.0f ) > kUnitScaleTolerance )
            return Common::MakeError<Result>(
                 std::format( "the entity's world scale is ({}, {}, {}), a ragdoll needs one: its bodies are "
                              "authored in centimetres of the rig",
                              entityScale.x, entityScale.y, entityScale.z ) );

        Animation::ComponentPose component( skeleton, pose );
        Result                   parts;
        parts.reserve( desc.Parts.size() );
        for ( const RagdollPartDesc& part : desc.Parts )
            parts.push_back( SplitRigid( entityWorld * component.Get( part.SkeletonBone ) ) );
        return Common::MakeSuccess( std::move( parts ) );
    }

    Common::ResultStr<std::vector<Animation::BoneOverride>>
    RagdollBoneOverrides( const RagdollDesc& desc, const Animation::Skeleton& skeleton,
                          const Animation::LocalPose& animated, const glm::mat4& entityWorld,
                          std::span<const RagdollPartTransform> parts )
    {
        using Result = std::vector<Animation::BoneOverride>;
        if ( const std::string fault = CheckPose( desc, skeleton, animated ); !fault.empty() )
            return Common::MakeError<Result>( fault );
        if ( parts.size() != desc.Parts.size() )
            return Common::MakeError<Result>(
                 std::format( "{} body transforms for a ragdoll of {} parts", parts.size(), desc.Parts.size() ) );

        Animation::ComponentPose component( skeleton, animated );
        const glm::mat4          worldToComponent = glm::inverse( entityWorld );
        Result                   overrides;
        overrides.reserve( parts.size() );
        for ( size_t i = 0; i < parts.size(); ++i )
        {
            const RagdollPartDesc& part      = desc.Parts[i];
            const glm::vec3        boneScale = ColumnLengths( component.Get( part.SkeletonBone ) );
            const glm::mat4        bodyWorld =
                 glm::translate( glm::mat4( 1.0f ), parts[i].Position ) * glm::mat4_cast( parts[i].Rotation );
            const glm::mat4 boneComponent =
                 worldToComponent * bodyWorld * glm::scale( glm::mat4( 1.0f ), boneScale );

            auto decomposed = Animation::BoneTransform::FromMatrix( boneComponent );
            if ( !decomposed )
                return Common::MakeError<Result>( std::format( "the body of bone '{}' does not decompose: {}",
                                                               part.Bone, decomposed.GetError() ) );
            Animation::BoneOverride entry;
            entry.Bone      = part.SkeletonBone;
            entry.Transform = decomposed.GetValue();
            overrides.push_back( entry );
        }
        return Common::MakeSuccess( std::move( overrides ) );
    }
} // namespace Desert::Physics
