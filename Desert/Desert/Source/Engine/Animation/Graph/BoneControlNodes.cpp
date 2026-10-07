#include "BoneControlNodes.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TwoBoneIKControl.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <format>

namespace Desert::Animation::Graph
{
    namespace
    {
        glm::vec3 ToVec3( const std::array<float, 3>& v )
        {
            return { v[0], v[1], v[2] };
        }

        Common::ResultStr<uint32_t> BoneOf( const Skeleton& skeleton, const std::string& name, const char* role )
        {
            if ( name.empty() )
                return Common::MakeError<uint32_t>( std::format( "names no {}", role ) );
            const auto bone = skeleton.FindBoneIndex( name );
            if ( !bone )
                return Common::MakeError<uint32_t>(
                     std::format( "names {} '{}', which this rig ({} bones) does not have", role, name,
                                  skeleton.GetBones().size() ) );
            return Common::MakeSuccess( *bone );
        }
    } // namespace

    Common::ResultStr<glm::vec3> ComponentSpaceTarget( const BoneControlTarget& target, const Skeleton& skeleton,
                                                       ComponentPose& component )
    {
        const glm::vec3 position = ToVec3( target.Position );
        if ( target.Bone.empty() )
            return Common::MakeSuccess( position );
        auto bone = BoneOf( skeleton, target.Bone, "target bone" );
        if ( !bone )
            return Common::MakeError<glm::vec3>( bone.GetError() );
        return Common::MakeSuccess( glm::vec3( component.Get( bone.GetValue() ) * glm::vec4( position, 1.0F ) ) );
    }

    Common::BoolResultStr ApplyTwoBoneIKNode( const TwoBoneIKNode& node, const Skeleton& skeleton, float alpha,
                                              LocalPose& pose, BoneControlScratch& scratch )
    {
        alpha = std::clamp( alpha, 0.0F, 1.0F );
        if ( alpha <= 0.0F )
            return Common::MakeSuccess( true );

        auto end = BoneOf( skeleton, node.EndBone, "end bone" );
        if ( !end )
            return Common::MakeError<bool>( std::format( "Two Bone IK {}", end.GetError() ) );
        const uint32_t joint = skeleton.ResolveParent( end.GetValue() );
        const uint32_t root = joint == Skeleton::NO_PARENT ? Skeleton::NO_PARENT : skeleton.ResolveParent( joint );
        if ( root == Skeleton::NO_PARENT )
            return Common::MakeError<bool>(
                 std::format( "Two Bone IK: end bone '{}' has no parent and grandparent to close a two-bone chain",
                              node.EndBone ) );

        ComponentPose component( skeleton, pose );
        auto          goal = ComponentSpaceTarget( node.Goal, skeleton, component );
        if ( !goal )
            return Common::MakeError<bool>( std::format( "Two Bone IK goal {}", goal.GetError() ) );
        auto pole = ComponentSpaceTarget( node.PoleTarget, skeleton, component );
        if ( !pole )
            return Common::MakeError<bool>( std::format( "Two Bone IK pole target {}", pole.GetError() ) );

        scratch.Overrides.clear();
        Solvers::TwoBoneIKSolution solution;
        if ( auto solved = SolveTwoBoneIKChain( skeleton, component, root, joint, end.GetValue(),
                                                Solvers::TwoBoneIKGoal{ goal.GetValue(), pole.GetValue() },
                                                solution, scratch.Overrides );
             !solved )
            return Common::MakeError<bool>(
                 std::format( "Two Bone IK on '{}' {}", node.EndBone, solved.GetError() ) );
        scratch.Before.clear();
        return ApplyBoneOverrides( skeleton, pose, component, scratch.Overrides, alpha, scratch.Before );
    }

    Common::BoolResultStr ApplyLookAtNode( const LookAtNode& node, const Skeleton& skeleton, float alpha,
                                           LocalPose& pose, BoneControlScratch& scratch )
    {
        alpha = std::clamp( alpha, 0.0F, 1.0F );
        if ( alpha <= 0.0F )
            return Common::MakeSuccess( true );

        auto bone = BoneOf( skeleton, node.Bone, "bone" );
        if ( !bone )
            return Common::MakeError<bool>( std::format( "Look At {}", bone.GetError() ) );

        ComponentPose component( skeleton, pose );
        auto          target = ComponentSpaceTarget( node.Target, skeleton, component );
        if ( !target )
            return Common::MakeError<bool>( std::format( "Look At target {}", target.GetError() ) );

        auto current = BoneTransform::FromMatrix( component.Get( bone.GetValue() ) );
        if ( !current )
            return Common::MakeError<bool>(
                 std::format( "Look At: bone '{}' in component space: {}", node.Bone, current.GetError() ) );

        constexpr float MIN_LENGTH_CM = 1.0e-4F;
        BoneTransform   aimed         = current.GetValue();
        const glm::vec3 axis          = aimed.Rotation * ToVec3( node.AimAxis );
        const glm::vec3 toTarget      = target.GetValue() - aimed.Translation;
        // A target on the bone or a zero axis names no direction: refused, the pose left bit-identical.
        if ( glm::length( axis ) < MIN_LENGTH_CM || glm::length( toTarget ) < MIN_LENGTH_CM )
            return Common::MakeError<bool>( std::format(
                 "Look At on '{}': {} names no direction", node.Bone,
                 glm::length( axis ) < MIN_LENGTH_CM ? "a zero aim axis" : "a target on the bone itself" ) );
        aimed.Rotation = glm::rotation( glm::normalize( axis ), glm::normalize( toTarget ) ) * aimed.Rotation;

        scratch.Overrides.clear();
        scratch.Overrides.push_back( { bone.GetValue(), aimed } );
        scratch.Before.clear();
        return ApplyBoneOverrides( skeleton, pose, component, scratch.Overrides, alpha, scratch.Before );
    }
} // namespace Desert::Animation::Graph
