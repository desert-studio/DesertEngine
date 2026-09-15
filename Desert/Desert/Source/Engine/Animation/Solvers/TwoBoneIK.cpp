#include "TwoBoneIK.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Animation::Solvers
{
    namespace
    {
        // 1 world unit = 1 cm, so this is one micrometre. Below it a "limb" carries no direction that survives
        // normalisation in float, and a chain that short cannot be seen at any camera distance this engine
        // renders at — so refusing is not a lost feature, it is the only honest answer.
        constexpr float MIN_LENGTH_CM = 1.0e-4F;

        /// Any unit vector perpendicular to `dir`, chosen from the world axis `dir` leans on LEAST so the
        /// cross product is far from zero. Only reached when the input chain is perfectly straight AND the
        /// pole target is on the goal line — see TwoBoneIKPlane::Arbitrary.
        glm::vec3 AnyPerpendicular( const glm::vec3& dir )
        {
            const glm::vec3 absolute( std::fabs( dir.x ), std::fabs( dir.y ), std::fabs( dir.z ) );
            glm::vec3       axis( 0.0F, 0.0F, 1.0F );
            if ( absolute.x <= absolute.y && absolute.x <= absolute.z )
            {
                axis = glm::vec3( 1.0F, 0.0F, 0.0F );
            }
            else if ( absolute.y <= absolute.z )
            {
                axis = glm::vec3( 0.0F, 1.0F, 0.0F );
            }
            return glm::normalize( glm::cross( dir, axis ) );
        }

        /// The component of `delta` perpendicular to the unit vector `dir`. Its LENGTH is what the caller
        /// tests: a short one means `delta` is (within a micrometre) parallel to `dir` and names no plane.
        glm::vec3 PerpendicularPart( const glm::vec3& delta, const glm::vec3& dir )
        {
            return delta - glm::dot( delta, dir ) * dir;
        }
    } // namespace

    TwoBoneIKSolution SolveTwoBoneIK( const TwoBoneIKChain& chain, const TwoBoneIKGoal& goal )
    {
        TwoBoneIKSolution out;
        out.Joint       = chain.Joint;
        out.End         = chain.End;
        out.UpperLength = glm::length( chain.Joint - chain.Root );
        out.LowerLength = glm::length( chain.End - chain.Joint );

        if ( out.UpperLength < MIN_LENGTH_CM || out.LowerLength < MIN_LENGTH_CM )
        {
            // NOT AN ASSERT AND NOT A ZERO POSE. A rig can legitimately arrive here mid-import, or from a
            // clip that scales a bone to nothing for one frame; the chain that came in is the best pose
            // available and handing it back unchanged is the only answer that cannot make the frame worse.
            out.Reach = TwoBoneIKReach::DegenerateChain;
            out.Plane = TwoBoneIKPlane::NotChosen;
            return out;
        }

        const glm::vec3 toGoal   = goal.Position - chain.Root;
        const float     goalDist = glm::length( toGoal );
        if ( goalDist < MIN_LENGTH_CM )
        {
            // The goal sits ON the root. There is no root->goal direction to build anything from, and every
            // choice of one is equally arbitrary — including UE's, which substitutes (1,0,0) and produces a
            // chain that snaps to the X axis for as long as the goal stays put. Keeping the input pose is
            // the answer that says "this input carries no instruction" instead of inventing one.
            out.Reach = TwoBoneIKReach::GoalAtRoot;
            out.Plane = TwoBoneIKPlane::NotChosen;
            return out;
        }

        const glm::vec3 dir = toGoal / goalDist;

        // ---- the bend plane -------------------------------------------------------------------------
        glm::vec3 bend = PerpendicularPart( goal.PoleTarget - chain.Root, dir );
        out.Plane      = TwoBoneIKPlane::FromPoleTarget;
        if ( glm::length( bend ) < MIN_LENGTH_CM )
        {
            bend      = PerpendicularPart( chain.Joint - chain.Root, dir );
            out.Plane = TwoBoneIKPlane::FromCurrentPose;
        }
        if ( glm::length( bend ) < MIN_LENGTH_CM )
        {
            bend      = AnyPerpendicular( dir );
            out.Plane = TwoBoneIKPlane::Arbitrary;
        }
        bend = glm::normalize( bend );

        // ---- the reach, as ONE clamp rather than three branches --------------------------------------
        //
        // A two-bone chain reaches the spherical shell [ |upper - lower|, upper + lower ] around the root.
        // Clamping the goal DISTANCE into that shell and then running the law of cosines once handles both
        // misses and the hit with the same four lines, keeps the bend plane in the clamped cases (so a goal
        // drifting out of reach straightens the chain smoothly instead of snapping it onto the goal line),
        // and makes the two arguments to `acos` bounded by construction.
        //
        // That last part is not a detail. UE recovers the joint's offset along the goal line as
        // `sqrt(upper^2 - (upper*sin(angle))^2)` and carries a guard saying "sometimes Xbox release produces
        // -0.f, causing ProjJointDist to be NaN" (`TwoBoneIK.cpp:174-176`), plus a separate
        // `bReverseUpperBone` sign flip for the case where the upper limb must point AWAY from the goal.
        // `upper * cos(angle)` is the same number, is signed already, and has no square root to go negative.
        const float minReach  = std::fabs( out.UpperLength - out.LowerLength );
        const float maxReach  = out.UpperLength + out.LowerLength;
        const float reachDist = std::clamp( goalDist, minReach, maxReach );

        out.Reach = TwoBoneIKReach::Reached;
        if ( goalDist > maxReach )
        {
            out.Reach = TwoBoneIKReach::ClampedFar;
        }
        else if ( goalDist < minReach )
        {
            out.Reach = TwoBoneIKReach::ClampedNear;
        }

        // `reachDist` is at least MIN_LENGTH_CM here: it is either goalDist (checked above) or a shell radius,
        // and a zero minReach can only be selected when goalDist itself is below it, which cannot happen.
        const float cosAngle = std::clamp(
             ( out.UpperLength * out.UpperLength + reachDist * reachDist - out.LowerLength * out.LowerLength ) /
                  ( 2.0F * out.UpperLength * reachDist ),
             -1.0F, 1.0F );
        const float sinAngle = std::sqrt( std::max( 0.0F, 1.0F - cosAngle * cosAngle ) );

        out.Joint = chain.Root + ( out.UpperLength * cosAngle ) * dir + ( out.UpperLength * sinAngle ) * bend;
        out.End   = chain.Root + reachDist * dir;
        return out;
    }

    const char* ToString( TwoBoneIKReach reach )
    {
        switch ( reach )
        {
            case TwoBoneIKReach::Reached:
                return "Reached";
            case TwoBoneIKReach::ClampedFar:
                return "ClampedFar";
            case TwoBoneIKReach::ClampedNear:
                return "ClampedNear";
            case TwoBoneIKReach::GoalAtRoot:
                return "GoalAtRoot";
            case TwoBoneIKReach::DegenerateChain:
                return "DegenerateChain";
        }
        return "?";
    }

    const char* ToString( TwoBoneIKPlane plane )
    {
        switch ( plane )
        {
            case TwoBoneIKPlane::FromPoleTarget:
                return "FromPoleTarget";
            case TwoBoneIKPlane::FromCurrentPose:
                return "FromCurrentPose";
            case TwoBoneIKPlane::Arbitrary:
                return "Arbitrary";
            case TwoBoneIKPlane::NotChosen:
                return "NotChosen";
        }
        return "?";
    }
} // namespace Desert::Animation::Solvers
