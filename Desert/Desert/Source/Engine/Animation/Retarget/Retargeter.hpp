#pragma once

/**
 * RETARGETING AS A FIXED ORDERED PIPELINE — T6.2.
 *
 * Three stages, in one order, written out in `Retarget()` as three calls. No op list, no registration,
 * no dispatch: that is R8, and the gap analysis argues it rather than asserting it —
 *
 *     "`FInstancedStruct` ops with a four-phase parent/child protocol is the right answer for an engine
 *      that must stay data-compatible across versions and let licensees insert passes. One engine, one
 *      team — a fixed ordered pipeline is the same behaviour with none of the dispatch, and the 5.8
 *      upgrade path spells that pipeline out."
 *
 * The order comes from that upgrade path, and each stage depends on the one before it:
 *
 *     1. PELVIS MOTION      the retargeted root position. Everything below inherits it, so it is first.
 *     2. FK CHAINS          the rotation delta, and the chain parameterisation. Needs the pelvis placed.
 *     3. IK CHAINS          the normalised limb extension, solved by Tier 2's `SolveTwoBoneIK`. Needs
 *                           the FK result, because the FK bend is what chooses the solve's plane.
 *
 * ── THE ONE WARNING T6.1 PAID FOR: THE DELTA GOES ON THE ROTATION, NOT THE TRANSFORM ─────────────────
 *
 * `08_retarget_measurement.md` §2.1 measured `JPH::SkeletonMapper`, whose equation is ours —
 * `out[b] = (source[a] * sourceInitial[a]^-1) * targetInitial[b]` — applied to the FULL transform:
 *
 *     "The equation applies the delta to the FULL transform, translation included. UE applies it only to
 *      the ROTATION and takes translation from its own rest pose — which is why the target's bone lengths
 *      are inviolable BY CONSTRUCTION. [...] worst limb-length error 1.325 % at k=1.25, 0.390 % at
 *      k=1.5, 7.934 % at k=3.0 — and NOT MONOTONIC in the proportion difference, so retarget error
 *      cannot be bounded by testing one pair of rigs."
 *
 * So `StageFKChains` writes ROTATIONS ONLY. Every target bone's local translation and scale stay exactly
 * as its own retarget pose has them, which makes limb length preservation a property of the data flow
 * rather than a number to be checked. The one deliberate exception is the pelvis, whose translation IS
 * the thing stage 1 retargets.
 *
 * The same measurement is why the suite takes several ratios INCLUDING ADJACENT ONES (1.25 and 1.5): a
 * non-monotonic error is not bounded by its endpoints.
 *
 * ── CURRENCY: `LocalPose` IN, `LocalPose` OUT, AND NOT ONE MATRIX BETWEEN THEM ───────────────────────
 *
 * T5.4 made the rig a `LocalPose -> LocalPose` operator and this is the same shape, for the same reason
 * plus one more that §3.1 of the measurement made concrete: `BoneTransform::FromMatrix` REFUSES on a
 * non-positive determinant, and a mirrored bone has one. A retargeter that resolved poses as `glm::mat4`
 * and decomposed them back would refuse every frame on an ordinary character. `Retarget/ModelPose.hpp`
 * is the answer — TRS all the way through, so there is no decomposition to fail.
 *
 * ── WHAT IS REFUSED, AND WHY EVERY ONE OF THEM IS LOAD-BEARING ───────────────────────────────────────
 *
 * T5.4's header states the rule this file inherits: *"a stage that provably cannot change the pose
 * passes every assertion a working one passes"*. A retargeter with nothing mapped emits the target's own
 * retarget pose, every frame, successfully. So `Initialize` refuses:
 *
 *   - a setup with no mapped bone and no chain — it could only ever emit the rest pose;
 *   - a pelvis bone either rig does not have, or one at zero height, where the height scale is undefined;
 *   - a chain whose end bone is not a descendant of its start bone on the rig it names;
 *   - a chain marked for IK whose target run is not exactly three bones, because Tier 2 ships a TWO-BONE
 *     solver and an N-bone chain silently solved as its first three is a wrong answer, not a partial one;
 *   - two chains claiming the same target bone, because "which one wins" has no answer invented here.
 *
 * ── WHAT IS DELIBERATELY NOT HERE ────────────────────────────────────────────────────────────────────
 *
 * No asset, no component, no serialisation and no panel. T6.2 is the retargeting LAYER; an authored
 * `.retarget` asset and the ECS hop that plays a foreign clip on a character are separate work with
 * separate decisions (which asset owns the rig pair, what the editor shows). Naming that remainder is
 * the point — it is not "done with a remainder", it is a boundary drawn at the layer the tier names.
 *
 * Scale is NOT retargeted: the target keeps its own retarget-pose scale. A source clip's scale keys
 * describe the SOURCE rig's proportions, and carrying them onto a differently proportioned target is the
 * proportion defect this whole task exists to remove. Measured, not assumed — see the suite.
 */

#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Retarget/ModelPose.hpp>
#include <Engine/Animation/Retarget/RetargetPose.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Animation::Retarget
{
    /**
     * @brief One authored chain: a run of bones on each rig, and whether its tip is driven by IK.
     *
     * `{ChainName, StartBone, EndBone, IKGoalName}` from §4.4, with the goal reduced to a bool. We have
     * no IK Rig asset for a goal to be named in, and the only goal a retarget chain can have is its own
     * tip — a named goal that can only ever resolve to one place is a second copy of the chain's end
     * bone, and the contract has one source of truth per value.
     */
    struct RetargetChain
    {
        std::string Name;
        std::string SourceStartBone;
        std::string SourceEndBone;
        std::string TargetStartBone;
        std::string TargetEndBone;

        /// FK alone reproduces the source's JOINT ANGLES. On a target whose limb segments are scaled
        /// unevenly against the source's, equal angles do NOT give equal reach, and this is the stage
        /// that fixes it: the goal is placed at the source's normalised extension of the TARGET's own
        /// rest chain length. Both values are exercised and measured; it is authored data, not a dial.
        bool DriveWithIK = false;
    };

    /// Everything a rig pair needs, gathered so `Initialize` takes one argument that can be authored.
    struct RetargetSetup
    {
        std::string SourcePelvisBone;
        std::string TargetPelvisBone;

        RetargetPose SourceRetargetPose;
        RetargetPose TargetRetargetPose;

        std::vector<RetargetChain> Chains;

        /// Target bone name -> source bone name, for bones whose names differ. Everything else maps by
        /// exact name, which is `JPH::SkeletonMapper::CanMapJoint`'s default and UE's starting point.
        /// Fuzzy matching lives in `Editor::FuzzyMatch` and moving it into the engine is a layering
        /// decision, not this task (`08_retarget_measurement.md` §4).
        std::map<std::string, std::string> BoneRenames;
    };

    /**
     * @brief A resolved chain, as the pipeline caches it. Public so the suite can measure a stage's
     *        inputs rather than only its outputs.
     */
    struct ResolvedChain
    {
        std::string           Name;
        std::vector<uint32_t> SourceRun; ///< start..end inclusive, parent before child
        std::vector<uint32_t> TargetRun;
        std::vector<float>    SourceParams; ///< 0 at the run's first bone, 1 at its last
        std::vector<float>    TargetParams;
        float                 SourceRestLength = 0.0F;
        float                 TargetRestLength = 0.0F;
        bool                  DriveWithIK      = false;
    };

    /// A 1:1 correspondence, in the target's resolve order.
    struct BonePairing
    {
        uint32_t TargetBone = 0;
        uint32_t SourceBone = 0;
    };

    /**
     * @brief Built once per (source rig, target rig) pair; run once per frame.
     *
     * The cache shape is the third thing `08_retarget_measurement.md` §6 says to take from Jolt: the
     * mapper is built once per rig pair and bakes its joint correspondence. Everything expensive to
     * decide — name matching, chain resolution, rest lengths, chain parameters, the pelvis height scale
     * — is decided in `Initialize`, and `Retarget` allocates nothing after the first call.
     *
     * Holds REFERENCES to both skeletons. They outlive it: a skeleton is owned by its `SkeletonAsset`
     * and a retargeter is a per-pair object built from two of them.
     */
    class Retargeter
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize( const Skeleton& source, const Skeleton& target,
                                                        RetargetSetup setup );

        /**
         * @brief The source rig's pose, as the target rig's pose. The whole task, in one call.
         *
         * WRITES INTO THE CALLER'S POSE, like `ControlRigStage::Evaluate` and for the same reason: this
         * runs once per character per frame, and a returned `LocalPose` is a heap allocation on every one
         * of them. `targetPose` is resized to the target rig if it is not already.
         *
         * Refuses an uninitialised retargeter and a source pose that is not the source rig's size. Every
         * other degenerate input has a defined, named outcome rather than a refusal — a source limb
         * collapsed to a point puts the IK goal on the chain root, and `SolveTwoBoneIK` reports
         * `GoalAtRoot` and leaves the chain exactly as FK placed it.
         */
        [[nodiscard]] Common::BoolResultStr Retarget( const LocalPose& sourceLocal, LocalPose& targetPose );

        /// target pelvis height / source pelvis height, both measured in their own retarget poses.
        [[nodiscard]] float GetPelvisHeightScale() const
        {
            return m_PelvisHeightScale;
        }

        [[nodiscard]] const std::vector<BonePairing>& GetBonePairings() const
        {
            return m_Pairings;
        }

        [[nodiscard]] const std::vector<ResolvedChain>& GetChains() const
        {
            return m_Chains;
        }

        /// `TargetInitial` — the target rig's bind pose with its retarget pose applied. Every bone's
        /// translation and scale in the output come from here unchanged, except the pelvis's.
        [[nodiscard]] const LocalPose& GetTargetInitialPose() const
        {
            return m_TargetInitialLocal;
        }

        [[nodiscard]] const LocalPose& GetSourceInitialPose() const
        {
            return m_SourceInitialLocal;
        }

    private:
        [[nodiscard]] Common::BoolResultStr ResolveChains();
        [[nodiscard]] Common::BoolResultStr BuildPairings();

        /// STAGE 1. The target pelvis's model-space translation for this frame.
        [[nodiscard]] glm::vec3 StagePelvisMotion( const ModelPose& sourceModel ) const;

        /// STAGE 2. Rotations for every target bone, and the pelvis translation stage 1 decided.
        [[nodiscard]] Common::BoolResultStr StageFKChains( const ModelPose& sourceModel,
                                                           const glm::vec3& pelvisModelTranslation );

        /// STAGE 3. Normalised limb extension, solved onto the target's own reach.
        [[nodiscard]] Common::BoolResultStr StageIKChains( const ModelPose& sourceModel );

        /// The source chain's FK delta at normalised position `param` along it — UE's
        /// `GetTransformAtChainParam`, and the one thing that lets a 3-bone source arm drive a 5-bone
        /// target arm instead of only re-aiming the first bone (`08_retarget_measurement.md` §4).
        [[nodiscard]] glm::quat SourceChainDeltaAt( const ResolvedChain& chain, float param,
                                                    const ModelPose& sourceModel ) const;

        static constexpr uint32_t NO_SOURCE = UINT32_MAX;
        static constexpr int32_t  NO_CHAIN  = -1;

        const Skeleton* m_Source = nullptr;
        const Skeleton* m_Target = nullptr;

        RetargetSetup m_Setup;

        LocalPose m_SourceInitialLocal;
        LocalPose m_TargetInitialLocal;
        ModelPose m_SourceInitialModel;
        ModelPose m_TargetInitialModel;

        std::vector<BonePairing> m_Pairings;
        std::vector<ResolvedChain> m_Chains;

        /// Per target bone, so stage 2 is one pass over the resolve order rather than a lookup per bone.
        std::vector<uint32_t> m_SourceOfTarget;
        std::vector<int32_t>  m_ChainOfTarget;
        std::vector<float>    m_ParamOfTarget;

        uint32_t m_SourcePelvis = 0;
        uint32_t m_TargetPelvis = 0;
        float    m_PelvisHeightScale = 1.0F;

        /// Scratch, kept so a per-frame retarget allocates nothing after the first.
        LocalPose m_WorkLocal;
        ModelPose m_WorkModel;

        bool m_Initialized = false;
    };
} // namespace Desert::Animation::Retarget
