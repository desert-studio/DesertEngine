#pragma once

/**
 * THE SOURCE RIG, OWNED — what makes a retargeter attachable to an Animator.
 *
 * `Retargeter` deliberately holds NEITHER skeleton. Its header states why: "an `Animation::Skeleton` is
 * owned by a `SkeletonAsset` and an asset unload frees it; a retargeter holding one across frames would
 * owe the pointer census an argument about outliving that unload, and the honest form of that argument is
 * 'it does not'". Both rigs are therefore passed to `Retarget()` on every call.
 *
 * An Animator has the TARGET rig — it holds a reference to the one its mesh owns, and the mesh outlives
 * it. It does not have the SOURCE rig, and cannot: the source rig belongs to a `SkeletonAsset` reached
 * through the retarget's dependency, and eviction releases that asset the moment no scene names it.
 *
 * ── SO THE BONES ARE COPIED, AND THAT IS THE DECISION ────────────────────────────────────────────────
 *
 * The alternatives were a raw pointer and a `shared_ptr` to the asset. The pointer register states the
 * principle this follows outright — *"a raw pointer deleted is worth more than a raw pointer argued
 * for"* — and a `shared_ptr` would be worse than either: it would make an Animator keep a source rig
 * resident, so `AssetEviction`'s reachability walk would be telling the truth about what is reachable and
 * still never able to release it, which is a leak that looks like correct behaviour.
 *
 * A COPY COSTS WHAT IT COSTS AND THE NUMBER IS SMALL: a `Skeleton` is its `BoneInfo` vector plus the
 * name map and the resolve order derived from it, made ONCE per (re)build — not per frame — and a build
 * happens only when the handle, the file, or either rig's signature moves. A 65-bone humanoid is on the
 * order of tens of kilobytes, against a retarget that is rebuilt perhaps once a session.
 *
 * ── IT CARRIES THE STAMP OF WHAT IT WAS BUILT FROM, AND THAT IS NOT WHERE `.derig` PUT IT ────────────
 *
 * `ControlRigComponent`'s rebuild trigger lives in three fields on `AnimationComponent`
 * (`BuiltRigSource` / `BuiltRigRevision` / `BuiltRigSignature`), which `SyncControlRig::forget()` has to
 * zero by hand on every early return — five of them. A missed one is a stamp that outlives the thing it
 * describes, and nothing in the type says it must not.
 *
 * Here the stamp is a member of the built object, so it is created and destroyed with it and there is no
 * `forget()` to get wrong. A duplicated entity inherits no stamp because it inherits no Animator. FOUR
 * facts rather than three, because a retarget has a second side: the handle, the file's revision, the
 * TARGET rig's signature, and the SOURCE rig's signature — the last of which moves when the source
 * character is re-exported and nothing else in the chain notices.
 */

#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Retarget/Retargeter.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace Desert::Animation::Retarget
{
    class RetargetSource
    {
    public:
        /**
         * @brief Builds the pair, or says why it could not.
         *
         * @param source          the rig the clips are authored on, BY VALUE — see the file note.
         * @param target          the rig being posed. Not kept: the Animator already holds it.
         * @param assetHandle     the `.retarget` this was built from, as a stamp. Never dereferenced.
         * @param assetRevision   that asset's revision at build time.
         *
         * Every structural refusal comes out of `Retargeter::Initialize` underneath — a setup that could
         * only ever emit the rest pose, a pelvis at zero height, a chain whose end does not descend from
         * its start, an IK run that is not two bones, two chains on one target bone.
         *
         * THERE IS DELIBERATELY NO "IS THIS THE RIGHT SOURCE RIG" CHECK HERE, and the absence is decided
         * rather than skipped. The only value it could compare is the skeleton's signature, and
         * `Skeleton::ComputeSignature` hashes names and parents alone — so it is EQUAL across the
         * proportion variants a retarget exists to bridge, and a check on it would pass exactly when it
         * mattered. Which rig is the source is settled where it is stated: `RetargetAsset` resolves the
         * path its file names, and a path has one answer.
         */
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<RetargetSource>>
        Create( Skeleton source, const Skeleton& target, RetargetSetup setup, uint64_t assetHandle,
                uint32_t assetRevision );

        [[nodiscard]] const Skeleton& GetSourceSkeleton() const
        {
            return m_Source;
        }

        [[nodiscard]] Retargeter& GetRetargeter()
        {
            return m_Retargeter;
        }

        [[nodiscard]] const Retargeter& GetRetargeter() const
        {
            return m_Retargeter;
        }

        /**
         * @brief What this pipeline emits when the SOURCE clip is at rest — the pose everything a layer
         *        adds is relative to.
         *
         * NOT `Retargeter::GetTargetInitialPose()`, AND THE DIFFERENCE WAS MEASURED RATHER THAN ARGUED.
         * The obvious answer is the target's own retarget rest, and it is wrong whenever stage 3 is not
         * the identity at rest: the IK stage places the tip at the SOURCE's normalised chain extension,
         * and that ratio is scale-invariant only under a UNIFORM proportion difference. On this project's
         * own corpus rig (x1.5 / x1.75 / x1.3) the retargeted rest sits 4.29 cm from the target's rest,
         * so an additive layer measured against `TargetInitial` injects that 4.29 cm into every frame —
         * and an additive layer of a clip that drives NOTHING moves the character, which is the shape a
         * one-line test catches and a reasoned-about reference does not.
         *
         * Computed once, in `Create`, because it is a constant of the pair.
         */
        [[nodiscard]] const LocalPose& GetRetargetedRest() const
        {
            return m_RetargetedRest;
        }

        /// The buffer the base clip is sampled into before it is retargeted. Sized to the SOURCE rig and
        /// kept, so a per-frame retarget allocates nothing.
        [[nodiscard]] LocalPose& SourceScratch()
        {
            return m_SourceScratch;
        }

        /// A second target-sized buffer, for the layer stack: a layer's own retargeted pose, which is then
        /// folded over the base. Separate from the evaluated pose because the fold reads both at once.
        [[nodiscard]] LocalPose& LayerScratch()
        {
            return m_LayerScratch;
        }

        /**
         * @brief Retarget @p sourceLocal onto @p out, remembering a refusal instead of repeating it.
         *
         * THE ONE CALL SITE OF `Retargeter::Retarget` IN THE ANIMATOR, used by the source stage and by
         * every layer, so the two cannot come to handle a refusal differently. Logs only when the message
         * CHANGES — `ControlRigStage::ReportRefusal`'s rule and its reason: a retarget that refuses does so
         * on every frame, and sixty identical lines a second is a log nobody reads, which is the same
         * silence wearing a different hat.
         *
         * Returns false and leaves @p out untouched on a refusal. The caller must not treat that as an
         * empty success: the pose it did not write is the previous stage's, and saying so is the whole
         * difference between this and a `static_cast<void>`.
         */
        [[nodiscard]] bool Run( const Skeleton& target, const LocalPose& sourceLocal, LocalPose& out );

        /// The last refusal, or empty. Kept for the reason `ControlRigStage::GetLastError` is: a per-frame
        /// stage cannot throw and an editor needs somewhere to read why nothing moved.
        [[nodiscard]] const std::string& GetLastError() const
        {
            return m_LastError;
        }

        /**
         * @brief Is this still the retargeter those four facts ask for?
         *
         * ASKED, NOT REMEMBERED BY THE CALLER. The whole point of putting the stamp here is that the
         * question has one answer and it lives with the object being asked about.
         */
        [[nodiscard]] bool IsBuiltFrom( uint64_t assetHandle, uint32_t assetRevision, uint64_t sourceSignature,
                                        uint64_t targetSignature ) const
        {
            return m_AssetHandle == assetHandle && m_AssetRevision == assetRevision &&
                   m_SourceSignature == sourceSignature && m_TargetSignature == targetSignature;
        }

        [[nodiscard]] uint64_t GetAssetHandle() const
        {
            return m_AssetHandle;
        }

    private:
        explicit RetargetSource( Skeleton source ) : m_Source( std::move( source ) )
        {
        }

        Skeleton   m_Source;
        Retargeter m_Retargeter;
        LocalPose  m_SourceScratch;
        LocalPose  m_LayerScratch;
        LocalPose  m_RetargetedRest;

        std::string m_LastError;
        std::string m_LastLoggedError;

        uint64_t m_AssetHandle     = 0;
        uint32_t m_AssetRevision   = 0;
        uint64_t m_SourceSignature = 0;
        uint64_t m_TargetSignature = 0;
    };
} // namespace Desert::Animation::Retarget
