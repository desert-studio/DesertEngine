// THE SOLVER, TESTED WITH FIVE POINTS AND NOTHING ELSE.
//
// This whole file links `Solvers/TwoBoneIK.cpp` and no other engine source. That is not a boast about
// coupling, it is the property the module was split out for: every degenerate input below — a goal past the
// limb's reach, a goal inside the fold radius, a goal ON the root, a zero-length limb, a pole target on the
// goal line — is a named behaviour rather than an assert, and pinning a named behaviour must not require a
// rig, a mesh, a GPU or a frame. Report 03 §783 names the layering; `Geometry::AutoRig` is the precedent in
// this tree.
//
// TWO INVARIANTS ARE CHECKED ON EVERY CASE, including the refusals, because they are what "the solver never
// hands back a broken chain" means:
//   * both limb LENGTHS are preserved (a solver that stretches a bone to reach is a different feature);
//   * no coordinate is NaN (UE carries a guard for exactly one float path that produces one).

#include <Engine/Animation/Solvers/TwoBoneIK.hpp>

#include <gtest/gtest.h>

#include <cmath>

using Desert::Animation::Solvers::SolveTwoBoneIK;
using Desert::Animation::Solvers::ToString;
using Desert::Animation::Solvers::TwoBoneIKChain;
using Desert::Animation::Solvers::TwoBoneIKGoal;
using Desert::Animation::Solvers::TwoBoneIKPlane;
using Desert::Animation::Solvers::TwoBoneIKReach;
using Desert::Animation::Solvers::TwoBoneIKSolution;

namespace
{
    // 1 world unit = 1 cm. A bent arm with EXACT limb lengths — 40 cm upper (0,32,24), 30 cm lower
    // (0,18,-24) — bending towards +Z, with the end 50 cm straight up from the root. Exact so that the
    // reach shell is exactly [10, 70] cm and the expectations below are readable numbers rather than
    // whatever a Pythagorean accident produced.
    TwoBoneIKChain BentArm()
    {
        return TwoBoneIKChain{ glm::vec3( 0.0F, 0.0F, 0.0F ), glm::vec3( 0.0F, 32.0F, 24.0F ),
                               glm::vec3( 0.0F, 50.0F, 0.0F ) };
    }

    float Upper( const TwoBoneIKChain& chain )
    {
        return glm::length( chain.Joint - chain.Root );
    }
    float Lower( const TwoBoneIKChain& chain )
    {
        return glm::length( chain.End - chain.Joint );
    }

    bool Finite( const glm::vec3& v )
    {
        return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
    }

    // The two invariants every outcome owes, asserted in one place so no case can quietly skip them.
    ::testing::AssertionResult ChainIsIntact( const TwoBoneIKChain& in, const TwoBoneIKSolution& out )
    {
        if ( !Finite( out.Joint ) || !Finite( out.End ) )
        {
            return ::testing::AssertionFailure()
                   << "solver produced a non-finite position (" << ToString( out.Reach ) << ")";
        }

        const float upper = glm::length( out.Joint - in.Root );
        const float lower = glm::length( out.End - out.Joint );
        if ( std::fabs( upper - Upper( in ) ) > 1e-3F )
        {
            return ::testing::AssertionFailure()
                   << "upper limb changed length: " << Upper( in ) << " cm -> " << upper << " cm";
        }
        if ( std::fabs( lower - Lower( in ) ) > 1e-3F )
        {
            return ::testing::AssertionFailure()
                   << "lower limb changed length: " << Lower( in ) << " cm -> " << lower << " cm";
        }
        return ::testing::AssertionSuccess();
    }
} // namespace

TEST( TwoBoneIKSolver, ReachableGoalIsHitExactlyAndTheBendFollowsThePole )
{
    const TwoBoneIKChain chain = BentArm();
    // 50 cm along +Y: inside [ |40-30| , 40+30 ] = [10, 70], so it is reachable.
    const TwoBoneIKGoal goal{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( 0.0F, 25.0F, 100.0F ) };

    const auto solved = SolveTwoBoneIK( chain, goal );

    EXPECT_EQ( solved.Reach, TwoBoneIKReach::Reached );
    EXPECT_EQ( solved.Plane, TwoBoneIKPlane::FromPoleTarget );
    EXPECT_TRUE( ChainIsIntact( chain, solved ) );
    EXPECT_NEAR( glm::length( solved.End - goal.Position ), 0.0F, 1e-3F )
         << "a reachable goal must be reached, not approached";
    EXPECT_GT( solved.Joint.z, 0.0F ) << "the joint must bend towards the pole target (+Z), not away from it";

    // The same goal with the pole mirrored must mirror the joint and leave the end exactly where it was.
    const TwoBoneIKGoal mirrored{ goal.Position, glm::vec3( 0.0F, 25.0F, -100.0F ) };
    const auto          other = SolveTwoBoneIK( chain, mirrored );
    EXPECT_LT( other.Joint.z, 0.0F );
    EXPECT_NEAR( other.Joint.z, -solved.Joint.z, 1e-3F );
    EXPECT_NEAR( glm::length( other.End - solved.End ), 0.0F, 1e-4F );
}

TEST( TwoBoneIKSolver, TheMeasuredLimbLengthsAreReportedBack )
{
    const TwoBoneIKChain chain  = BentArm();
    const auto           solved = SolveTwoBoneIK(
         chain, TwoBoneIKGoal{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( 0.0F, 25.0F, 100.0F ) } );

    // The lengths are an OUTPUT, not an input — the call site cannot pass one that disagrees with the chain.
    EXPECT_NEAR( solved.UpperLength, Upper( chain ), 1e-4F );
    EXPECT_NEAR( solved.LowerLength, Lower( chain ), 1e-4F );
}

TEST( TwoBoneIKSolver, AGoalBeyondReachStraightensTheChainAndClampsTheEndToTheReachableSphere )
{
    const TwoBoneIKChain chain = BentArm(); // reach 70 cm
    const TwoBoneIKGoal  goal{ glm::vec3( 0.0F, 500.0F, 0.0F ), glm::vec3( 0.0F, 25.0F, 100.0F ) };

    const auto solved = SolveTwoBoneIK( chain, goal );

    EXPECT_EQ( solved.Reach, TwoBoneIKReach::ClampedFar );
    EXPECT_TRUE( ChainIsIntact( chain, solved ) );

    // THE END IS ON THE SPHERE, NOT ON THE GOAL. Leaving it at the goal is how a limb visibly detaches.
    EXPECT_NEAR( glm::length( solved.End - chain.Root ), 70.0F, 1e-3F );
    EXPECT_NEAR( solved.End.y, 70.0F, 1e-3F );
    EXPECT_NEAR( solved.Joint.z, 0.0F, 1e-3F ) << "a fully extended chain has no bend left";
    EXPECT_NEAR( solved.Joint.y, 40.0F, 1e-3F );
}

TEST( TwoBoneIKSolver, AGoalInsideTheFoldRadiusFoldsAsFarAsTheChainGoes )
{
    const TwoBoneIKChain chain = BentArm(); // fold radius |40 - 30| = 10 cm
    const TwoBoneIKGoal  goal{ glm::vec3( 0.0F, 2.0F, 0.0F ), glm::vec3( 0.0F, 25.0F, 100.0F ) };

    const auto solved = SolveTwoBoneIK( chain, goal );

    // THIS IS THE CASE UE DOES NOT NAME. Its cosine clamp silently produces a chain whose lower limb has to
    // stretch to meet the goal; here the miss has a name, and the two limb lengths still hold.
    EXPECT_EQ( solved.Reach, TwoBoneIKReach::ClampedNear );
    EXPECT_TRUE( ChainIsIntact( chain, solved ) );
    EXPECT_NEAR( glm::length( solved.End - chain.Root ), 10.0F, 1e-3F );
}

TEST( TwoBoneIKSolver, AGoalOnTheRootLeavesTheChainWhereItWas )
{
    const TwoBoneIKChain chain = BentArm();
    const TwoBoneIKGoal  goal{ chain.Root, glm::vec3( 0.0F, 25.0F, 100.0F ) };

    const auto solved = SolveTwoBoneIK( chain, goal );

    // No root->goal direction exists. UE substitutes (1,0,0) and snaps the chain onto the X axis; the pose
    // that came in carries strictly more information than an arbitrary axis does.
    EXPECT_EQ( solved.Reach, TwoBoneIKReach::GoalAtRoot );
    EXPECT_EQ( solved.Plane, TwoBoneIKPlane::NotChosen );
    EXPECT_EQ( solved.Joint, chain.Joint );
    EXPECT_EQ( solved.End, chain.End );
}

TEST( TwoBoneIKSolver, ALimbWithNoLengthIsNamedRatherThanDividedBy )
{
    const TwoBoneIKGoal goal{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( 0.0F, 25.0F, 100.0F ) };

    const TwoBoneIKChain collapsedUpper{ glm::vec3( 0.0F ), glm::vec3( 0.0F ), glm::vec3( 0.0F, 30.0F, 0.0F ) };
    const auto           a = SolveTwoBoneIK( collapsedUpper, goal );
    EXPECT_EQ( a.Reach, TwoBoneIKReach::DegenerateChain );
    EXPECT_TRUE( ChainIsIntact( collapsedUpper, a ) );

    const TwoBoneIKChain collapsedLower{ glm::vec3( 0.0F ), glm::vec3( 0.0F, 40.0F, 0.0F ),
                                         glm::vec3( 0.0F, 40.0F, 0.0F ) };
    const auto           b = SolveTwoBoneIK( collapsedLower, goal );
    EXPECT_EQ( b.Reach, TwoBoneIKReach::DegenerateChain );
    EXPECT_TRUE( ChainIsIntact( collapsedLower, b ) );
}

TEST( TwoBoneIKSolver, APoleTargetOnTheGoalLineFallsBackToTheChainsOwnBendRatherThanAnArbitraryAxis )
{
    const TwoBoneIKChain chain = BentArm(); // bends towards +Z
    // The pole target sits exactly on the root->goal line, so it names no plane at all.
    const TwoBoneIKGoal onLine{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( 0.0F, 20.0F, 0.0F ) };

    const auto solved = SolveTwoBoneIK( chain, onLine );

    EXPECT_EQ( solved.Plane, TwoBoneIKPlane::FromCurrentPose );
    EXPECT_TRUE( ChainIsIntact( chain, solved ) );
    EXPECT_GT( solved.Joint.z, 0.0F ) << "the authored bend direction (+Z) must survive a useless pole target";

    // AND IT IS STABLE ACROSS THE LINE. An arbitrary-axis fallback does not merely look wrong, it FLIPS:
    // a pole target drifting across the goal line would put the knee in two different places on consecutive
    // frames. Approaching the line from either side must give the same answer as being on it.
    const TwoBoneIKGoal fromOneSide{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( 1e-6F, 20.0F, 0.0F ) };
    const TwoBoneIKGoal fromTheOther{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( -1e-6F, 20.0F, 0.0F ) };
    EXPECT_NEAR( SolveTwoBoneIK( chain, fromOneSide ).Joint.z, solved.Joint.z, 1e-3F );
    EXPECT_NEAR( SolveTwoBoneIK( chain, fromTheOther ).Joint.z, solved.Joint.z, 1e-3F );
}

TEST( TwoBoneIKSolver, AStraightChainWithAUselessPoleTargetSaysTheAxisWasArbitrary )
{
    // Perfectly straight AND a pole target on the goal line: the one configuration that carries no bend
    // information anywhere. The answer still has to be a valid chain, and it has to SAY that it was a guess.
    const TwoBoneIKChain straight{ glm::vec3( 0.0F ), glm::vec3( 0.0F, 40.0F, 0.0F ),
                                   glm::vec3( 0.0F, 70.0F, 0.0F ) };
    const TwoBoneIKGoal  onLine{ glm::vec3( 0.0F, 50.0F, 0.0F ), glm::vec3( 0.0F, 10.0F, 0.0F ) };

    const auto solved = SolveTwoBoneIK( straight, onLine );

    EXPECT_EQ( solved.Plane, TwoBoneIKPlane::Arbitrary );
    EXPECT_EQ( solved.Reach, TwoBoneIKReach::Reached );
    EXPECT_TRUE( ChainIsIntact( straight, solved ) );
    EXPECT_NEAR( glm::length( solved.End - onLine.Position ), 0.0F, 1e-3F );
}

TEST( TwoBoneIKSolver, TheUpperLimbMayPointAWAYFromTheGoal )
{
    // A long upper limb and a short lower one, with the goal close in: the elbow has to end up FURTHER from
    // the goal than the root is. UE reaches this case through a `bReverseUpperBone` sign flip on a square
    // root; here the signed cosine gives it with no branch, and that is what this test pins.
    const TwoBoneIKChain chain{ glm::vec3( 0.0F ), glm::vec3( 0.0F, 90.0F, 5.0F ),
                                glm::vec3( 0.0F, 110.0F, 0.0F ) };
    const TwoBoneIKGoal  goal{ glm::vec3( 0.0F, 75.0F, 0.0F ), glm::vec3( 0.0F, 40.0F, 100.0F ) };

    const auto solved = SolveTwoBoneIK( chain, goal );

    EXPECT_EQ( solved.Reach, TwoBoneIKReach::Reached );
    EXPECT_TRUE( ChainIsIntact( chain, solved ) );
    EXPECT_GT( solved.Joint.y, goal.Position.y ) << "the joint must be allowed past the goal along the goal line";
}

TEST( TwoBoneIKSolver, EveryOutcomeAndEveryPlaneHasAName )
{
    // A `ToString` that returns "?" for a value the enum can hold is a log line that cannot be read back,
    // and both enums are printed into refusals an artist is meant to act on.
    for ( const auto reach : { TwoBoneIKReach::Reached, TwoBoneIKReach::ClampedFar, TwoBoneIKReach::ClampedNear,
                               TwoBoneIKReach::GoalAtRoot, TwoBoneIKReach::DegenerateChain } )
    {
        EXPECT_STRNE( ToString( reach ), "?" );
    }
    for ( const auto plane : { TwoBoneIKPlane::FromPoleTarget, TwoBoneIKPlane::FromCurrentPose,
                               TwoBoneIKPlane::Arbitrary, TwoBoneIKPlane::NotChosen } )
    {
        EXPECT_STRNE( ToString( plane ), "?" );
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
