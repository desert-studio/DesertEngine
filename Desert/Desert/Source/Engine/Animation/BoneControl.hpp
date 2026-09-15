#pragma once

/**
 * THE SKELETAL-CONTROL CONTRACT: A SOLVER RETURNS A SPARSE LIST OF OVERRIDES, AND THE BASE DOES THE BLEND.
 *
 * Report 03 §0 calls this "the one design decision everything else follows from", and the reason it is
 * worth copying is not the solver it makes possible but the twenty it makes INTERCHANGEABLE. Four
 * consequences, all of which this file is written to keep:
 *
 *   1. ALPHA IS FREE FOR EVERY SOLVER. Not one line of blending lives in a solver. `Evaluate` below owns it,
 *      which is why a half-applied IK is guaranteed to be a sane pose rather than twenty solvers' worth of
 *      slightly different opinions about what "half" means.
 *   2. ORDERING IS A CONTRACT, NOT A CONVENTION. Overrides arrive parents-before-children and `Evaluate`
 *      REFUSES a list that is not, naming the two bones. UE spells the same requirement as a `check()` in
 *      `LocalBlendCSBoneTransforms` (`BonePose.h:777-788`).
 *   3. COST IS PROPORTIONAL TO BONES TOUCHED. A two-bone solver on a hundred-bone rig writes three
 *      transforms and reads three parent chains. The other ninety-seven bones are not visited, not copied
 *      and not converted — and `Tests/Engine/BoneControlContract` asserts that rather than assuming it.
 *   4. A CONTROL CANNOT TAKE OVER THE FRAME. UE marks `EvaluateComponentSpace_AnyThread` and
 *      `Update_AnyThread` `final`; here `Evaluate` is simply not virtual, which is the same statement with
 *      no vtable slot to get wrong. The extension points are the three protected virtuals and nothing else.
 *
 * THE OVERRIDES ARE COMPONENT-SPACE AND THE BLEND IS LOCAL-SPACE, and those are not in tension — they are
 * the two halves of the reason this shape exists. A solver has to work in component space because a goal is
 * a POSITION and positions do not exist in a parent-relative pose. A blend has to happen in local space
 * because blending component transforms shears a chain: at alpha 0.5 the child would sit half-way between
 * two places its parent never is. А1 chose local for `BlendedBaseLocal` on the same grounds and А1's merge
 * measured the difference at 254.96 cm on the witness rig — this is that decision applied to a second site,
 * not a new one.
 */

#include <Engine/Animation/Pose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;

    /**
     * @brief One bone's replacement transform, in COMPONENT (mesh-local) space.
     *
     * A `BoneTransform` rather than a `glm::mat4` so the override is in the same currency as the pose it
     * will become — see Pose.hpp on why the matrix round trip was worth deleting. It costs one decompose per
     * overridden bone per frame (three, for two-bone IK) and it is the decompose that REFUSES a mirrored or
     * collapsed basis instead of silently straightening it.
     */
    struct BoneOverride
    {
        static constexpr uint32_t NO_BONE = UINT32_MAX;

        uint32_t      Bone = NO_BONE;
        BoneTransform Transform;
    };

    /**
     * @brief Writes `overrides` into `pose`, blended by `alpha` IN LOCAL SPACE.
     *
     * Free rather than a member so the contract's load-bearing half is testable without constructing a
     * control at all: the sparseness, the ordering refusal and the two alpha endpoints are properties of
     * THIS function, and `Tests/Engine/BoneControlContract` exercises it directly with hand-built lists.
     *
     * `component` must be a view over `pose` (the Animator's is). It is re-invalidated after every write
     * because a written bone changes the component transform of itself and of every descendant, and the
     * next override in the list may be one of those descendants — that is exactly why the list has to be
     * ordered. Invalidating the whole view rather than a subtree is a flag memset over the rig, which is
     * cheaper than the bookkeeping that would find the subtree, for any rig this engine will ever load.
     *
     * TWO PASSES, AND THE REASON IS WHAT ALPHA MEANS. Pass one applies every override at FULL strength, so
     * each child is converted against a parent the solver has already placed; pass two blends each of those
     * target locals with the local that was there. `alpha` is then exactly "how far from the input pose
     * towards the solved one", per bone, which is the only reading under which 0.5 is half of anything.
     *
     * `scratch` holds the pre-solve locals between the two passes. An out-parameter rather than a local so
     * a per-frame call allocates nothing after the first (report 02 §768: per-frame scratch belongs to the
     * pass, not to the node) — and so that this function stays free, and testable, with no control at all.
     *
     * Refuses, naming the bone, when: an index is out of range; the list is not ordered parents-first; or a
     * resulting local transform cannot be decomposed. A refusal leaves the pose as it found it — the range
     * and order checks run before anything is written, and a mid-chain decompose failure is rolled back.
     * Returns success having changed NOTHING when `alpha <= 0` — which is a specified answer, not an empty
     * one, and is what makes "alpha 0 is bit-exact pass-through" a property a frame can be diffed against.
     */
    [[nodiscard]] Common::BoolResultStr ApplyBoneOverrides( const Skeleton& skeleton, LocalPose& pose,
                                                            ComponentPose&                   component,
                                                            const std::vector<BoneOverride>& overrides,
                                                            float alpha, std::vector<BoneTransform>& scratch );

    /// What a control IS, for the one question a caller legitimately asks of a base pointer: "is the control
    /// already on this animator the kind I am about to configure?". A checked alternative to `dynamic_cast`
    /// on a hot path, and the string the Details panel shows.
    enum class BoneControlKind : uint8_t
    {
        TwoBoneIK,
    };

    [[nodiscard]] const char* ToString( BoneControlKind kind );

    /**
     * @brief The base every skeletal control derives from. See the file comment for why it looks like this.
     */
    class BoneControl
    {
    public:
        BoneControl()          = default;
        virtual ~BoneControl() = default;

        BoneControl( const BoneControl& )            = delete;
        BoneControl& operator=( const BoneControl& ) = delete;

        [[nodiscard]] virtual BoneControlKind GetKind() const = 0;

        /**
         * @brief THE ONE ENTRY POINT, AND IT IS NOT VIRTUAL.
         *
         * Runs the gate, the solve and the blend in that order. A derived class cannot replace it, reorder
         * it, or skip the blend — which is what makes "every control honours alpha the same way" a fact
         * about the type rather than a rule twenty authors have to remember.
         */
        [[nodiscard]] Common::BoolResultStr Evaluate( const Skeleton& skeleton, LocalPose& pose,
                                                      ComponentPose& component );

        /**
         * @brief Bind authored bone NAMES to indices on this rig. Explicit, and called on rig change.
         *
         * Report 03 §1.4: UE resolves bone references in `CacheBones_AnyThread`, before recursing, on every
         * change of the bone container — never lazily inside the solve. The reason is the one A1 wrote
         * `BoneRef` for: a solver must not pay a string lookup inside its own loop, and a name that does not
         * resolve is a report to an artist, not a per-frame branch.
         */
        [[nodiscard]] virtual Common::BoolResultStr Resolve( const Skeleton& skeleton ) = 0;

        /// 0 = the input pose is passed through untouched, 1 = the solve replaces it. Clamped on read, not
        /// on write, so a caller that stores 1.5 gets told 1 rather than having its own value rewritten.
        [[nodiscard]] float GetAlpha() const;
        void                SetAlpha( float alpha );

        /// The last refusal this control produced, or empty. Kept so the editor can show WHY a control is
        /// doing nothing — the failure of a solve is per-frame and would otherwise only exist in a log line
        /// that scrolls past.
        [[nodiscard]] const std::string& GetLastError() const
        {
            return m_LastError;
        }

    protected:
        /**
         * @brief The solve. Appends component-space overrides, parents before children, and touches the pose
         *        through nothing but `component` (read-only use).
         *
         * MUST NOT return success with an empty list — `Evaluate` treats that as the refusal it is. A
         * control that legitimately has nothing to do says so through `Resolve`/alpha, not by producing
         * silence that looks like work.
         */
        [[nodiscard]] virtual Common::BoolResultStr Solve( const Skeleton& skeleton, ComponentPose& component,
                                                           std::vector<BoneOverride>& out ) = 0;

    private:
        /// Logs `m_LastError` only when it DIFFERS from the last message logged — see m_ReportedError.
        void ReportErrorOnChange();

        /// Reused between frames so a solve allocates nothing after the first. UE keeps the same array for
        /// the same reason and says so: "Resused bone transform array to avoid reallocating in skeletal
        /// controls" (`AnimNode_SkeletalControlBase.h:133`).
        std::vector<BoneOverride> m_Overrides;

        /// The pre-solve local transforms, between ApplyBoneOverrides' two passes. Reused for the same
        /// reason m_Overrides is.
        std::vector<BoneTransform> m_Scratch;

        float       m_Alpha = 1.0F;
        std::string m_LastError;

        /// The last message that was LOGGED. A refused control refuses every frame — an unresolvable bone
        /// name does not repair itself — so logging `m_LastError` unconditionally would be sixty identical
        /// lines a second, which is how a real diagnostic becomes something people filter out. Logging only
        /// on change reports the condition once and reports its end once.
        std::string m_ReportedError;
    };
} // namespace Desert::Animation
