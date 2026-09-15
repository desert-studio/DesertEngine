#include "TwoBoneIKControl.hpp"

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Desert::Animation
{
    namespace
    {
        /// The component-space transform of `bone`, as TRS. Refuses for the same reason `FromMatrix` does.
        Common::ResultStr<BoneTransform> ComponentTransformOf( const Skeleton& skeleton, ComponentPose& component,
                                                               uint32_t bone )
        {
            auto decomposed = BoneTransform::FromMatrix( component.Get( bone ) );
            if ( !decomposed.IsSuccess() )
            {
                return Common::MakeFormattedError<BoneTransform>( "bone {} ('{}') in component space: {}", bone,
                                                                  skeleton.GetBones()[bone].Name,
                                                                  decomposed.GetError() );
            }
            return decomposed;
        }

        /// The rotation that takes `from` to `to`, or identity when either is too short to have a direction.
        /// A DELTA, PREMULTIPLIED ONTO THE EXISTING ROTATION — never an absolute orientation, so whatever
        /// twist the animator authored about the limb's own axis survives the solve (report 03 §3.4 item 6).
        glm::quat RotationBetween( const glm::vec3& from, const glm::vec3& to )
        {
            constexpr float MIN_LENGTH_CM = 1.0e-4F;
            if ( glm::length( from ) < MIN_LENGTH_CM || glm::length( to ) < MIN_LENGTH_CM )
            {
                return { 1.0F, 0.0F, 0.0F, 0.0F };
            }
            return glm::rotation( glm::normalize( from ), glm::normalize( to ) );
        }
    } // namespace

    void TwoBoneIKControl::SetEndBone( std::string name )
    {
        m_EndBone.SetName( std::move( name ) );
        m_JointBone  = BoneOverride::NO_BONE;
        m_RootBone   = BoneOverride::NO_BONE;
        m_ChainError = "two-bone IK's end bone was renamed and has not been resolved against a rig since.";
    }

    Common::BoolResultStr TwoBoneIKControl::Resolve( const Skeleton& skeleton )
    {
        m_JointBone = BoneOverride::NO_BONE;
        m_RootBone  = BoneOverride::NO_BONE;

        // THE TWO PREDICATES ARE SEPARATE BECAUSE THE TWO FIXES ARE (A1's BoneRef, report 03 §784): an empty
        // field is "nobody filled this in", a set-but-unresolved field is "this rig does not have that bone",
        // and telling an artist the second when it is the first sends them looking at the wrong thing.
        if ( !m_EndBone.HasName() )
        {
            m_ChainError = "two-bone IK has no end bone authored, so there is no chain to solve.";
            return Common::MakeError<bool>( m_ChainError );
        }

        if ( !m_EndBone.Resolve( skeleton ) )
        {
            m_ChainError = fmt::format( "two-bone IK names end bone '{}', which this rig (signature {}, {} "
                                        "bones) does not have.",
                                        m_EndBone.GetName(), skeleton.GetSignature(), skeleton.GetBones().size() );
            return Common::MakeError<bool>( m_ChainError );
        }

        const uint32_t joint = skeleton.ResolveParent( m_EndBone.GetIndex() );
        if ( joint == Skeleton::NO_PARENT )
        {
            m_ChainError = fmt::format( "two-bone IK's end bone '{}' is a root: a two-bone chain is the named "
                                        "bone, its parent and its grandparent, and this one has neither.",
                                        m_EndBone.GetName() );
            return Common::MakeError<bool>( m_ChainError );
        }

        const uint32_t root = skeleton.ResolveParent( joint );
        if ( root == Skeleton::NO_PARENT )
        {
            m_ChainError = fmt::format( "two-bone IK's end bone '{}' has a parent ('{}') but no grandparent: "
                                        "the chain is one bone long, which has one segment and therefore no "
                                        "second limb to bend.",
                                        m_EndBone.GetName(), skeleton.GetBones()[joint].Name );
            return Common::MakeError<bool>( m_ChainError );
        }

        m_JointBone = joint;
        m_RootBone  = root;
        m_ChainError.clear();
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr TwoBoneIKControl::Solve( const Skeleton& skeleton, ComponentPose& component,
                                                   std::vector<BoneOverride>& out )
    {
        if ( !m_ChainError.empty() )
        {
            return Common::MakeError<bool>( m_ChainError );
        }

        const uint32_t endBone = m_EndBone.GetIndex();

        auto rootTransform = ComponentTransformOf( skeleton, component, m_RootBone );
        if ( !rootTransform.IsSuccess() )
        {
            return Common::MakeError<bool>( rootTransform.GetError() );
        }
        auto jointTransform = ComponentTransformOf( skeleton, component, m_JointBone );
        if ( !jointTransform.IsSuccess() )
        {
            return Common::MakeError<bool>( jointTransform.GetError() );
        }
        auto endTransform = ComponentTransformOf( skeleton, component, endBone );
        if ( !endTransform.IsSuccess() )
        {
            return Common::MakeError<bool>( endTransform.GetError() );
        }

        const Solvers::TwoBoneIKChain chain{ rootTransform.GetValue().Translation,
                                             jointTransform.GetValue().Translation,
                                             endTransform.GetValue().Translation };
        const Solvers::TwoBoneIKGoal  goal{ m_Goal, m_PoleTarget };

        const Solvers::TwoBoneIKSolution solution = Solvers::SolveTwoBoneIK( chain, goal );
        m_LastReach                               = solution.Reach;
        m_LastPlane                               = solution.Plane;

        if ( solution.Reach == Solvers::TwoBoneIKReach::GoalAtRoot ||
             solution.Reach == Solvers::TwoBoneIKReach::DegenerateChain )
        {
            // REFUSING RATHER THAN WRITING THE INPUT BACK, and the difference is measurable. Both outcomes
            // hand the chain back unmoved, so "apply them" would push three transforms through
            // decompose(inverse(parent) * compose(...)) and land a few ULPs away from where they started —
            // A1's merge measured that exact round trip as 12 stray pixels on the control rig. A refusal
            // leaves the pose bit-for-bit as the previous stage produced it, which is what a frame can be
            // diffed against.
            return Common::MakeFormattedError<bool>(
                 "two-bone IK on '{}' could not solve: {}. Goal ({}, {}, {}) cm, chain lengths {} / {} cm.",
                 m_EndBone.GetName(), Solvers::ToString( solution.Reach ), m_Goal.x, m_Goal.y, m_Goal.z,
                 solution.UpperLength, solution.LowerLength );
        }

        // ---- three overrides, parents first. The order is the contract (BoneControl.hpp). --------------

        // Upper limb: rotation only. Its own position is the chain's root and nothing may move it — an IK
        // solver that translated the shoulder would be detaching the arm from the body.
        BoneTransform upper = rootTransform.GetValue();
        upper.Rotation = RotationBetween( chain.Joint - chain.Root, solution.Joint - chain.Root ) * upper.Rotation;
        out.push_back( { m_RootBone, upper } );

        // Lower limb: rotated by the old->new direction delta AND moved onto the solved joint position.
        BoneTransform lower = jointTransform.GetValue();
        lower.Rotation =
             RotationBetween( chain.End - chain.Joint, solution.End - solution.Joint ) * lower.Rotation;
        lower.Translation = solution.Joint;
        out.push_back( { m_JointBone, lower } );

        // End bone: TRANSLATION ONLY. UE keeps the input rotation here too ("currently not doing anything to
        // rotation / keeping input rotation", `TwoBoneIK.cpp:55-59`) and the reason is that the end bone's
        // orientation is the ANIMATION's statement about the hand, not the goal's — a goal is a position.
        // The rotation policies UE layers on top (bTakeRotationFromEffectorSpace, bMaintainEffectorRelRot)
        // need a goal that carries an orientation, which this one deliberately does not; see the report.
        BoneTransform end = endTransform.GetValue();
        end.Translation   = solution.End;
        out.push_back( { endBone, end } );

        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation
