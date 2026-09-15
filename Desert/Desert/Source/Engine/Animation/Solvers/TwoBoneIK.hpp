#pragma once

/**
 * TWO-BONE IK, AS ARITHMETIC OVER POINTS — NO POSE, NO SKELETON, NO BONE.
 *
 * The layering is the lesson report 03 §748 draws from UE and it is the one worth copying: the algorithm
 * lives in a module that knows nothing about poses, nodes or bones (`AnimationCore`, whose whole public IK
 * surface is `TwoBoneIK.h` / `FABRIK.h` / `CCDIK.h`), and every node framework wraps it. The same split
 * already exists here for `Geometry::AutoRig`, so this is the tree's own habit rather than an import.
 *
 * What it buys, concretely: this file is unit-testable with five `glm::vec3`s and no mesh, no GPU, no
 * `Animator`, no clip and no asset. Every degenerate input below is pinned by a test that constructs three
 * points, and none of them needed a rig to exist.
 */

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Animation::Solvers
{
    /**
     * @brief Where the end effector ended up, and WHY. Deliberately not a bool and not a silent clamp.
     *
     * A two-bone chain cannot reach every point in space: it reaches the shell between |upper - lower| and
     * upper + lower around the root. Both misses are ordinary in production — a goal beyond reach is a leg
     * on a step too far, a goal inside the fold radius is a hand pushed into the chest — and they want
     * DIFFERENT reports to an artist and different follow-up work (soft IK for one, a goal clamp for the
     * other). A caller handed one "it did not quite reach" bit cannot tell them apart, so the solver names
     * which of the five things happened.
     */
    enum class TwoBoneIKReach : std::uint8_t
    {
        Reached,         ///< the end effector is ON the goal
        ClampedFar,      ///< the goal is further than upper + lower; the chain points at it, fully extended
        ClampedNear,     ///< the goal is closer than |upper - lower|; the chain folds as far as it can
        GoalAtRoot,      ///< goal and root coincide: no direction exists, so the chain is LEFT AS IT WAS
        DegenerateChain, ///< a limb has (almost) no length: there is nothing to rotate, chain left as it was
    };

    /**
     * @brief Which plane the chain was bent in — the pole target's, the pose's own, or an arbitrary one.
     *
     * A pole target is a POINT, and a point on the root->goal line names no plane at all (report 03 §785:
     * "a pole vector is a point, not a direction", converted to an in-plane direction inside the solver).
     * UE's fallback for that case is `FindBestAxisVectors` — an arbitrary axis derived from the goal
     * direction, which throws the animator's authored bend away and can flip a knee backwards between two
     * frames as the pole crosses the line. We fall back to the CHAIN'S OWN current bend first, which is the
     * pose that was authored, and only reach for an arbitrary axis when the input chain is itself perfectly
     * straight — the one configuration that genuinely carries no bend information.
     */
    enum class TwoBoneIKPlane : std::uint8_t
    {
        FromPoleTarget,  ///< the pole target is off the root->goal line and chose the plane
        FromCurrentPose, ///< the pole target named no plane; the chain's own bend was used instead
        Arbitrary,       ///< neither did; the input chain is straight, so any plane is as good as any other
        NotChosen,       ///< no plane was needed: the solve refused (GoalAtRoot / DegenerateChain)
    };

    /// The chain as three points in ONE space, in order. Which space is the caller's business — the solver
    /// only requires that all five points below live in the same one.
    struct TwoBoneIKChain
    {
        glm::vec3 Root  = glm::vec3( 0.0F );
        glm::vec3 Joint = glm::vec3( 0.0F );
        glm::vec3 End   = glm::vec3( 0.0F );
    };

    struct TwoBoneIKGoal
    {
        glm::vec3 Position   = glm::vec3( 0.0F );
        glm::vec3 PoleTarget = glm::vec3( 0.0F );
    };

    struct TwoBoneIKSolution
    {
        glm::vec3 Joint = glm::vec3( 0.0F );
        glm::vec3 End   = glm::vec3( 0.0F );

        TwoBoneIKReach Reach = TwoBoneIKReach::DegenerateChain;
        TwoBoneIKPlane Plane = TwoBoneIKPlane::NotChosen;

        /// The limb lengths the solve used, MEASURED FROM THE CHAIN rather than supplied. Reported because a
        /// caller that wants soft IK or stretching needs the same two numbers, and deriving them twice is
        /// how the two copies come to disagree.
        float UpperLength = 0.0F;
        float LowerLength = 0.0F;
    };

    /**
     * @brief Place the joint and the end effector so the chain reaches `goal.Position` if it can.
     *
     * THE LIMB LENGTHS ARE MEASURED FROM THE CHAIN, not passed in. UE's point solver takes them as
     * arguments and has four overloads because of it, two of which exist only to measure the lengths off
     * the pose first ("use actual sizes instead of ref skeleton, so we take into account translation and
     * scaling from other bone controllers", `TwoBoneIK.cpp:77-79`). A length argument that must equal
     * `|Joint - Root|` is a second copy of a value the caller already handed over, and the only thing it
     * can do is disagree with it. If stretching is ever wanted, it is a scale applied to the MEASURED
     * lengths and it belongs in a parameter of its own, not in a chance to pass the wrong length.
     *
     * NEVER FAILS AND NEVER DIVIDES BY ZERO. Every degenerate input has a named outcome above and returns a
     * chain that is geometrically valid — the two limb lengths are preserved in all five cases.
     */
    [[nodiscard]] TwoBoneIKSolution SolveTwoBoneIK( const TwoBoneIKChain& chain, const TwoBoneIKGoal& goal );

    [[nodiscard]] const char* ToString( TwoBoneIKReach reach );
    [[nodiscard]] const char* ToString( TwoBoneIKPlane plane );
} // namespace Desert::Animation::Solvers
