#pragma once

/**
 * CONTROLS: THE THING AN ANIMATOR TAKES HOLD OF.
 *
 * T5.1. A control is `offset(local) + pose(local) + shape-name`, parented to zero or more spaces with
 * WEIGHTS, and resolved lazily through those parents on read (report 01 §(a)1-3).
 *
 * ── NOT `BoneControl`, AND THE DIFFERENCE IS THE WHOLE POINT ─────────────────────────────────────────
 *
 * `Engine/Animation/BoneControl.hpp` (T2.1) is a SOLVER's write into a pose: a two-bone IK node computes
 * bone transforms and overrides them. Nobody grabs it. This file is the other half of the word: the
 * element an animator selects, drags and keys, which then drives bones. One is the output of maths, the
 * other is the input of a person, and they share a noun by accident of English.
 *
 * ── THE INVARIANT UE NEEDS AND WE DO NOT ─────────────────────────────────────────────────────────────
 *
 * Report 01 §(a)1 asks for "one dirty bit per slot, plus the invariant that local and global are never
 * both dirty". That invariant exists because `URigHierarchy` stores local AND global as authorable state
 * and has to remember which of the two is currently the truth (`RigHierarchyElements.h:415-419` carries an
 * `ensure` because Epic got it wrong once).
 *
 * WE DO NOT COPY IT, and the reason is A1's substrate rather than a shortcut: `LocalPose` made the local
 * side the only authored one and `ComponentPose` a derived cache with a per-bone converted flag. A cache
 * cannot be "dirty" in the sense that competes with the truth — it is either computed or not. So the
 * invariant holds HERE BY CONSTRUCTION, and the way to keep it is to make sure there is no path that
 * stores an authored global. `SetGlobalTransform` therefore BACK-SOLVES to local at the write and keeps
 * nothing; that is one function (`LocalFromGlobal`) rather than a second state machine, and the suite
 * asserts that a global written and read back is the same transform.
 *
 * The part of the report that IS copied is the one it warns about (§(c)1): dirtying is PROPAGATED to
 * dependents through a precomputed adjacency, not solved by invalidating everything. A control parented to
 * another control, or to a bone, must go stale when that parent moves — and only then.
 *
 * ── SPACE SWITCHING IS A WEIGHT ASSIGNMENT ───────────────────────────────────────────────────────────
 *
 * The consequence the tier exists for (report 01 §7). "Parent this hand to the world instead of to the
 * chest" is `SetSpaceWeights`, and the only extra work is the twenty lines that keep the control where it
 * was on screen — which are cheap only because the lazy resolver is already here. There is no
 * `SwitchParent` mechanism beside the weights, and that is the test: switching by assigning weights and
 * switching by a named call have to be the same operation, because there is only one.
 */

#include <Engine/Animation/Pose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;

    /// What one of a control's parent slots points at.
    enum class ControlSpaceKind : uint8_t
    {
        Component, ///< the mesh's own space — the "world" a rig has. Index is ignored.
        Bone,      ///< a bone of the skeleton this rig is built over, in component space
        Control,   ///< another control in this hierarchy, which must resolve first
    };

    /**
     * @brief One parent slot: what to follow, and how much.
     *
     * A WEIGHT AND NOT A BOOL, because that is what makes space switching an assignment. Zero means the
     * slot is inert but still declared — which is how a control can be blended between spaces over time
     * rather than snapping, and why removing a slot is a rig edit while zeroing it is an animation one.
     */
    struct ControlSpace
    {
        ControlSpaceKind Kind   = ControlSpaceKind::Component;
        uint32_t         Index  = 0;
        float            Weight = 1.0F;
    };

    /**
     * @brief A control, as the three things it is made of.
     *
     * `Offset` is the RIG AUTHOR'S: where this control's zero sits inside its parent space, so that an
     * animator's "no rotation" means the pose the rigger chose. `Pose` is the ANIMATED value and the only
     * thing a clip or a Sequencer key ever writes. Keeping them apart is what report 01 §(c)3 warns is
     * "a third transform layer that must be inverted correctly on every write path"; here there is exactly
     * one write path (`SetGlobalTransform`) and it inverts it once.
     *
     * `ShapeName` is a NAME resolved against a shape library, never geometry (§(a)6). A control that draws
     * nothing leaves it empty, and T5.2's manipulator layer is what will read it — this file must not know
     * what a shape looks like.
     *
     * `ShapeTransform` IS THE THIRD TERM, AND IT IS NOT `Offset`. UE composes a drawn control as
     * `LibraryShapeTransform * ControlShapeTransform * ControlGlobalTransform` (report 01 §(a)6); we had
     * the library's term and the control's global and nothing in between, so every built-in — authored at
     * unit size — drew one CENTIMETRE wide, because 1 world unit is 1 cm. The obvious alternative is
     * measured wrong rather than merely inelegant: sizing a control through `Offset.Scale` multiplies the
     * POSE's translation too, and a control sized 8x put its shape at 182.9 units where the animator had
     * asked for 26.4. So this is a separate value, it is read by NOTHING in this file, and that is the
     * property T5.2's suite asserts — resizing a control must leave its global bit-identical.
     */
    struct ControlElement
    {
        std::string               Name;
        std::string               ShapeName;
        BoneTransform             ShapeTransform;
        BoneTransform             Offset;
        BoneTransform             Pose;
        std::vector<ControlSpace> Parents;
    };

    /**
     * @brief The controls of one rig, and the resolution of their parent spaces.
     *
     * Built once (`Add` until it refuses), then evaluated per frame against the `ComponentPose` that says
     * where the bones currently are. The pose is passed to `Evaluate`, not held: it belongs to the
     * Animator and changes every frame, and a stored reference is how a rig ends up reading last frame's
     * skeleton.
     */
    class ControlHierarchy
    {
    public:
        static constexpr uint32_t INVALID = UINT32_MAX;

        /**
         * @brief Adds a control. Refuses a duplicate name, an unknown parent, or a parent cycle.
         *
         * REFUSES RATHER THAN REPAIRS. A rig whose control names collide has two things an animator can
         * select that are one thing when keyed, and report 01 §(c)8 records what that costs in UE: a
         * shipped repair path called `HACK_FixMultipleParamsWithSameName` that opens a modal dialog.
         * Names are the identity story, and it is settled here rather than in the channel story.
         */
        [[nodiscard]] Common::ResultStr<uint32_t> Add( ControlElement element );

        [[nodiscard]] size_t Size() const
        {
            return m_Controls.size();
        }

        [[nodiscard]] uint32_t Find( const std::string& name ) const;

        [[nodiscard]] const ControlElement& Get( uint32_t control ) const
        {
            return m_Controls[control];
        }

        /// The animated value. Writing it dirties this control and everything that follows it.
        [[nodiscard]] Common::BoolResultStr SetPose( uint32_t control, const BoneTransform& pose );

        /// The rig author's offset. Same propagation: an offset edit moves the control, which is exactly
        /// the case report 01 §(c)3 says editors get wrong ("controls that jump when their offset is
        /// authored") — here it jumps on purpose, at the moment the rigger changes it, and never later.
        [[nodiscard]] Common::BoolResultStr SetOffset( uint32_t control, const BoneTransform& offset );

        /**
         * @brief The control in component space, resolving its parents and itself if needed.
         *
         * LAZY, like `ComponentPose::Get`, and for the same measured reason: a manipulator draws the
         * handful of controls that are visible, a drag reads one, and only a bake reads all of them.
         */
        [[nodiscard]] glm::mat4 GetGlobalTransform( uint32_t control );

        /**
         * @brief Put the control THERE, whatever its parents are doing.
         *
         * The drag path (§(a)7): world delta -> global -> local through the inverse of the parent space
         * and the offset, written into `Pose`. NOTHING GLOBAL IS STORED — see the file note. The round
         * trip is the suite's assertion rather than a comment.
         */
        [[nodiscard]] Common::BoolResultStr SetGlobalTransform( uint32_t control, const glm::mat4& global );

        /**
         * @brief Assign the parent weights. THIS IS SPACE SWITCHING.
         *
         * @param keepWorld when true, the control's `Pose` is recomputed so it stays exactly where it was
         *        on screen. That is the whole of "switch space without the character twitching", and it is
         *        twenty lines only because the resolver above already exists (report 01 §(c)2).
         *
         * Refuses a weight set that is all zero: a control with no space is not "in world space", it is a
         * control whose parent transform is undefined, and answering identity would be a silent wrong
         * answer. Declare a `Component` slot for world.
         */
        [[nodiscard]] Common::BoolResultStr SetSpaceWeights( uint32_t control, const std::vector<float>& weights,
                                                             bool keepWorld );

        /**
         * @brief Point this hierarchy at the pose the bones are in this frame, and drop every cache.
         *
         * Called once per evaluation. Taking the pose here rather than in the constructor is what stops a
         * rig from reading last frame's skeleton: there is no stored pose to go stale.
         */
        void Evaluate( const Skeleton& skeleton, ComponentPose& pose );

        /// Whether `control`'s global has already been computed since the last thing that dirtied it. The
        /// flag the laziness claim is asserted through, as in `ComponentPose::Converted`.
        [[nodiscard]] bool Resolved( uint32_t control ) const;

        /// Controls that would go stale if `control` moved, in the order they must be re-resolved. The
        /// precomputed adjacency report 01 §(c)1 says dirty propagation depends on.
        [[nodiscard]] const std::vector<uint32_t>& Dependents( uint32_t control ) const;

        /**
         * @brief Empty when this rig resolved cleanly; the reason it did not, otherwise.
         *
         * A RIG IS BUILT AGAINST A SKELETON AND EVALUATED AGAINST ANOTHER — that is the failure this exists
         * for, and it is not exotic: a control names a bone by INDEX, and the same rig applied to a
         * character with fewer bones would silently resolve those spaces to identity and put every control
         * at the origin. Asked at `Evaluate`, because that is the first moment both halves are present, and
         * answered as a SENTENCE rather than a bool for the reason `Skeleton::GetStructureError` is one.
         */
        [[nodiscard]] const std::string& GetStructureError() const
        {
            return m_StructureError;
        }

    private:
        void Dirty( uint32_t control );

        /// The parent space of `control`: the weighted blend of its slots. Resolves what it needs.
        [[nodiscard]] glm::mat4 ParentSpace( uint32_t control );

        std::vector<ControlElement>        m_Controls;
        std::vector<std::vector<uint32_t>> m_Dependents;
        std::vector<glm::mat4>             m_Global;
        std::vector<uint8_t>               m_Resolved;

        /// The bone spaces this rig names, as `Evaluate` found them. A SNAPSHOT AND NOT A POINTER: the
        /// pose belongs to the Animator and changes every frame, and a class keeping a pointer into it
        /// would owe the ownership register an argument about lifetime that nothing here could honestly
        /// make. Indexed by bone; only the entries the rig uses are ever written.
        std::vector<glm::mat4> m_BoneSpace;
        std::string            m_StructureError;

        /// Scratch for the two iterative walks (dirty propagation, and collecting what a read needs).
        /// Members rather than locals for `ComponentPose::Get`'s reason: a per-frame re-evaluation
        /// allocates nothing after the first one.
        std::vector<uint32_t> m_Scratch;
        std::vector<uint32_t> m_Needed;
    };

    /**
     * @brief The weighted blend of several transforms, as a control's parent space.
     *
     * FREE AND NAMED because it is a decision, not a detail. Translation and scale are weighted sums;
     * rotation is a weighted accumulation of quaternions with sign alignment, normalised at the end —
     * i.e. an nlerp generalised to N inputs. It is not a slerp and does not claim to be: a slerp of three
     * rotations has no closed form, and the alternatives (a chain of pairwise slerps, or an iterative
     * average) are respectively order-dependent and a loop with a convergence criterion. Weights are
     * normalised by their sum, so "0.5 / 0.5" and "1 / 1" mean the same thing.
     *
     * TAKES TRANSFORMS AND NOT MATRICES, and that is a defect this function had and a census found. It
     * used to decompose each matrix itself and `continue` past one that refused — so a mirrored or
     * degenerate parent was dropped from the sum while its WEIGHT still counted in the normalisation, and
     * the answer came back quietly scaled towards nothing with no one told. The decomposition now happens
     * in the caller, which knows the control's NAME and can say so; what is left here cannot fail.
     *
     * Returns identity for an empty list, which only the caller's own refusal should be able to produce.
     */
    [[nodiscard]] glm::mat4 BlendSpaces( const std::vector<BoneTransform>& spaces,
                                         const std::vector<float>&         weights );
} // namespace Desert::Animation
