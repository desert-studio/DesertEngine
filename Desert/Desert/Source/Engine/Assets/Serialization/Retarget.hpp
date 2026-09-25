#pragma once

/**
 * THE RETARGET AS A FILE — T6.2 gets an author, and therefore a scene gets a retarget.
 *
 * Tier T6.2 is built: `Engine/Animation/Retarget/` is a three-stage pipeline measured against
 * `JPH::SkeletonMapper` on the same two rigs and the same two clips (`Docs/Animation/08_retarget_
 * measurement.md`), and it wins on every row — worst limb-length error 0.000024 % against 7.934 %, a
 * pelvis that rises by the ratio of the two rigs' heights instead of by 1.00, and a chain re-aimed along
 * its whole length instead of only at its start. AND NOTHING COULD CALL IT. `Retargeter::Initialize`
 * takes a `RetargetSetup` somebody has to fill in in C++, and nobody did.
 *
 * ── THE DEFECT SHAPE THIS EXISTS TO AVOID, AND IT IS THE THIRD INSTANCE THIS WEEK ────────────────────
 *
 * `AssetPreloader::PreloadCloudLayouts` scanned a directory, registered every `.dclayout` it found, and
 * was CALLED BY NOBODY. The feature was dead end to end from the day it shipped, and every test of the
 * format, of the bake and of the panel passed, because not one of them ran the LAYER. `.derig`
 * (`Serialization/ControlRig.hpp`) was the second instance and closed it for tier T5 the same way this
 * file closes it for T6.2.
 *
 * So the proof this file owes is NOT "the bytes round-trip". It is "a scene whose character names a
 * retarget produces DIFFERENT skinning matrices from the same scene without one", and
 * `Tests/Engine/RetargetAsset` measures exactly that, on a MOVING clip.
 *
 * WHY THE CLIP HAS TO BE MOVING, AND IT IS THE ONE TRAP T6.1 PAID FOR. §2.1 of the measurement:
 * "in the rest pose the error is zero for every k [...] a bind-pose screenshot of a retargeter built on
 * this mapper is flawless no matter how badly the rigs are matched". A retarget hop that is wired up
 * backwards, or not wired up at all, is INDISTINGUISHABLE from a working one in the bind pose — so a
 * test that samples tick 0 asserts nothing, and neither does a screenshot of a stopped character.
 *
 * ── WHERE THE PAIR LIVES, WHICH IS THE LOAD-BEARING DECISION ─────────────────────────────────────────
 *
 * A retarget is a statement about TWO rigs, not a property of one. Every field below — the two pelvis
 * names, every chain's four bone names, every rename, both retarget poses — is authored against a
 * specific source rig and a specific target rig, and means nothing without them. So the pair has to be
 * pinned somewhere, and there are exactly three places it could be: this file, the component, or
 * nowhere.
 *
 *   THE SOURCE RIG IS NAMED BY THIS FILE, BY PATH. If the source rig were named by the component instead,
 *   the same `.retarget` pointed at a different source rig would be accepted whenever the bone names
 *   happened to resolve, and would then be wrong by whatever the two rigs' proportions differ by — the
 *   "middle link drops a property" shape with a file in the middle.
 *
 *   AND IT IS A PATH RATHER THAN A SIGNATURE, WHICH IS THE OPPOSITE OF WHAT `.skmesh` DOES. A signature
 *   was the first answer here, because `SkinnedMeshAsset::ResolveDependencies` names its rig that way and
 *   the mechanism is already built. It is WRONG for this file, and measurably so:
 *   `Skeleton::ComputeSignature` hashes the sorted `name<parentName` pairs and NOTHING ELSE, so two
 *   exports of one character at different proportions have the SAME signature — which is precisely the
 *   pair a retarget exists to bridge (T6.1 measured k = 1.25, 1.5, 2.0, 3.0, all of them signature-equal).
 *   A signature-keyed lookup would therefore be free to bind the TARGET's own rig as the source, and the
 *   retarget would quietly become the identity: a feature that runs, reports success, and does nothing —
 *   the exact shape this file was written to end. `Tests/Engine/RetargetPipeline`'s own helper says so in
 *   as many words ("this rig has the SAME signature as its source").
 *
 *   The path is RELATIVE TO THE COOKED MESHES ROOT, joined in `RetargetAsset::ResolveDependencies` and
 *   nowhere else — the shape `.decloudtype` uses for its noise volume, against the root a `.skeleton`
 *   actually lives under (`AssetPreloader` scans skeletons from `MESH_PATH_COOKED` and from nowhere
 *   else). Relative, so the library is the same library on another machine.
 *
 *   THE TARGET RIG IS THE ENTITY'S OWN, AND IS NOT NAMED HERE. A second statement of it would be a
 *   second source of truth for which rig this entity has, and the loser of a disagreement between the
 *   file and the mesh would be invisible. It is the same answer `.derig` gives — a control rig names no
 *   skeleton at all and resolves its names against whichever rig it is applied to — and the target-side
 *   names below are resolved at build time by `BuildRetargeter`, which REFUSES and names the bone when
 *   the entity's rig does not have it.
 *
 * ── ITS OWN VERSION COUNTER, AND THE ARGUMENT IS THE PAYLOAD ─────────────────────────────────────────
 *
 * `PrefabData.hpp` states, in the field comment on its two integers, exactly WHY a `.deprefab` shares the
 * scene's number: "a prefab's payload is the scene's own EntityData, written by the same
 * ComponentRegistry, so a schema step that moves Core::kSceneVersion moves this file's format with it
 * whether anyone remembered prefabs or not". Not one clause of that is true here — a `.retarget` carries
 * no `EntityData`, no component key and nothing `ComponentRegistry` writes — so borrowing `kSceneVersion`
 * would tie this format to steps that cannot touch it and bind every existing scene to a re-run of the
 * migrator for a change that is not in them.
 *
 * `.anim` (`kAnimationVersion`) and `.derig` (`kControlRigVersion`) are the precedents that fit: distinct
 * animation-domain payloads with sequences of their own. `.detheme`, `.decloudtype` and `.derig` are the
 * SHAPE that fits: the version is stated by the file about itself (since v2 in the text asset header,
 * under `RTGT`), an unknown value is REFUSED by name rather than read as if it meant what it means here,
 * and there is no migration in the runtime at all - Tools/SceneMigrator raises the files.
 *
 * AND THERE IS NOTHING TO MIGRATE. Generation 1 is the first generation: no build has ever written a
 * `.retarget`, so the corpus conversion the contract requires around a version bump has an empty input.
 * The engine's scene and prefab corpora are untouched: adding a component KEY has never moved
 * `kSceneVersion` (A3/2 `cf9ab2d0` added `TwoBoneIK` and did not, A12 added `ControlRig` and did not),
 * because an unknown key is preserved by `Serialize/ForeignKeys.hpp` and an absent one defaults.
 *
 * ── EVERYTHING ELSE IS A NAME, FOR `.derig`'s REASON ─────────────────────────────────────────────────
 *
 * In memory a `ResolvedChain` holds bone INDICES, because that is what a per-frame retarget wants. In the
 * FILE every bone is a name, and the conversion happens at load against the two skeletons the retarget is
 * being built for. Storing indices would be the same defect with a file in the middle: both ends look
 * right — the retarget saves, the retarget loads — and a retarget applied to a re-exported character
 * silently drives the wrong bones.
 */

#include <Engine/Animation/Retarget/Retargeter.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser, the file dialog, the component's slot and the loader all agree
    /// on. One constant, because a second spelling of it is a slot that silently refuses a valid file.
    inline constexpr const char* kRetargetExtension = ".retarget";

    /// Where retargets live, RELATIVE to the project's assets root — the same relative form every asset
    /// slot is serialized in, so a scene that names one is portable off this machine.
    inline constexpr const char* kRetargetAssetsRelativeDir = "Retargets/";

    /**
     * @brief The FILE layout's generation.
     *
     *   1 - the source rig by relative path, the two pelvis names, both retarget poses, the chains and the
     *       renames (A25).
     *   2 - the text asset header (T7c): Kind "Retarget", the GUID that IS the retarget's identity and its
     *       handle (RetargetAsset's constructor), and this number under `RTGT`; the top-level FormatVersion
     *       is gone. A version-1 file is refused by name; Tools/SceneMigrator mints its GUID.
     *
     * See the file note for why this is its own sequence and not `Core::kSceneVersion`. An unknown value
     * is refused in BOTH directions rather than read as if it meant what it means here, and there is no
     * migration step in the runtime — the `.derig` / `.detheme` shape, borrowed on purpose.
     *
     * This counter's only power is to REFUSE, so the question "bump or not" is always exactly "must a file
     * written before the change be refused". The number moves the first time a `.retarget` written today
     * cannot be read as written; a field whose absence means what the build did before it existed is not
     * such a change.
     */
    inline constexpr int32_t kRetargetVersion = static_cast<int32_t>( Assets::kRetargetSchemaVersion );

    /// The subsystem versions a .retarget of this build states: the retarget schema, and nothing else.
    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> RetargetTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kRetargetSchemaTag,
                                                static_cast<uint32_t>( kRetargetVersion ) } };
        return versions;
    }

    /// EVERY SCALAR HERE CARRIES AN INITIALISER, for `Serialization/Animation.hpp`'s reason: these structs
    /// are what reflect-cpp writes to disk, and a field left indeterminate is bytes that outlive the
    /// process. glm's default constructors leave their components indeterminate, so they are spelled out.

    /**
     * @brief One authored rotation offset of one bone, in that bone's own local frame.
     *
     * AN ARRAY OF ROWS AND NOT A `map<string, quat>`, although the runtime form IS a map. The file is the
     * thing a rigger hand-edits and a diff reads, and an array gives two things a JSON object does not:
     * `ValidateRetargetData` can name the ROW that is wrong ("offset 3 names no bone"), and a duplicate
     * entry is REFUSABLE rather than silently resolved by whichever the parser saw last.
     */
    struct RetargetBoneOffsetData
    {
        std::string Bone;
        glm::quat   Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );

        [[nodiscard]] bool operator==( const RetargetBoneOffsetData& ) const = default;
    };

    /**
     * @brief One rig's retarget pose: what to rotate, and the one pelvis translation.
     *
     * `Retarget/RetargetPose.hpp` is the runtime form and this is its file form, field for field. There is
     * deliberately nothing else in it: §4.4 of the gap analysis describes UE's retarget pose as "a sparse
     * map<name, quat> of local deltas plus one pelvis translation", and T6.1 measured why it can be
     * nothing more — a target differing from the source only in rest orientation retargets with a worst
     * limb-length error of 2.98e-05 %, so an A-pose against a T-pose is absorbed exactly by the equation
     * and needs no special case.
     */
    struct RetargetPoseData
    {
        std::vector<RetargetBoneOffsetData> BoneOffsets;

        /// CENTIMETRES (1 world unit = 1 cm), in the pelvis bone's OWN local frame — which is what
        /// "raise this rig's hips" means. Everything below the pelvis follows through the chain.
        glm::vec3 PelvisOffset = glm::vec3( 0.0f );

        [[nodiscard]] bool operator==( const RetargetPoseData& ) const = default;
    };

    /**
     * @brief One authored chain: a run of bones on each rig, and whether its tip is driven by IK.
     *
     * The file form of `Animation::Retarget::RetargetChain`, field for field. `DriveWithIK` is authored
     * data and not a dial: FK alone reproduces the source's JOINT ANGLES, and on a target whose limb
     * segments are scaled unevenly against the source's, equal angles do not give equal reach. Both
     * values are exercised and measured by `Tests/Engine/RetargetPipeline`.
     */
    struct RetargetChainData
    {
        std::string Name;
        std::string SourceStartBone;
        std::string SourceEndBone;
        std::string TargetStartBone;
        std::string TargetEndBone;
        bool        DriveWithIK = false;

        [[nodiscard]] bool operator==( const RetargetChainData& ) const = default;
    };

    /**
     * @brief One bone whose name differs between the rigs.
     *
     * TARGET -> SOURCE, in that direction, because that is the direction the retarget is read in: for each
     * bone of the rig being posed, which bone of the clip's rig drives it. Everything not named here maps
     * by exact name, which is `JPH::SkeletonMapper::CanMapJoint`'s default and UE's starting point.
     */
    struct RetargetBoneRenameData
    {
        std::string TargetBone;
        std::string SourceBone;

        [[nodiscard]] bool operator==( const RetargetBoneRenameData& ) const = default;
    };

    /**
     * @brief One retarget on disk — the pair, and everything authored about it.
     *
     * `SourceSkeleton` IS THE SOURCE HALF OF THE PAIR and the field this format exists for; see the file
     * note for why it is a path and not a signature. Empty is refused: a retarget with no source rig can
     * only ever be the identity.
     */
    struct RetargetAssetData
    {
        /// The text asset header, FIRST so the registry reads it without parsing the rest. Absent only on
        /// a retarget that has never been written: WriteRetarget stamps it (the GUID kept, or minted when new).
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               Name;

        /// The `.skeleton` the CLIPS are authored on, RELATIVE to the cooked meshes root (e.g.
        /// "IKProbe.skeleton"). Resolved by `RetargetAsset::ResolveDependencies`, which performs the one
        /// join. See the file note for why this is not a signature.
        std::string SourceSkeleton;

        std::string SourcePelvisBone;
        std::string TargetPelvisBone;

        RetargetPoseData SourceRetargetPose;
        RetargetPoseData TargetRetargetPose;

        std::vector<RetargetChainData>      Chains;
        std::vector<RetargetBoneRenameData> BoneRenames;

        [[nodiscard]] bool operator==( const RetargetAssetData& ) const = default;
    };

    // ----------------------------------------------------------------------------------------------
    // Pure functions over the file form. No filesystem, no GPU, no globals.
    // ----------------------------------------------------------------------------------------------

    /**
     * @brief Rejects a retarget the loader cannot honour, naming the row that is wrong.
     *
     * Pure, so a retarget editor can refuse to save for the same reason the loader refuses to read, rather
     * than the two disagreeing about what is legal. Refuses: an empty, absolute or escaping source-rig
     * path; an empty pelvis name
     * on either side; a non-finite number anywhere; a non-normalisable rotation offset; an offset or
     * rename row with an empty bone name; two offsets on one bone; two renames claiming one target bone; a
     * chain with an empty name or an empty bone name; and two chains with the same name.
     *
     * WHAT IT DOES NOT CHECK IS EVERYTHING THAT NEEDS A RIG: whether a bone exists, whether a chain's end
     * descends from its start, whether an IK run is three bones long, whether two chains claim one target
     * bone, whether the pelvis sits at a non-zero height. Every one of those is already refused by
     * `Retargeter::Initialize`, by name, and re-deciding them here would be a second copy of the rule that
     * can come to disagree with the first. This function's job is the half of legality a file can settle
     * ON ITS OWN.
     */
    NO_DISCARD Common::BoolResultStr ValidateRetargetData( const RetargetAssetData& data );

    /**
     * @brief Parses the text of a `.retarget`, or says why it could not.
     *
     * Reads the version FIRST and on its own, for `ParseControlRig`'s reason: a file from another format
     * fails a full parse with a message about whichever field happens to be missing, which is true and
     * useless.
     */
    NO_DISCARD Common::ResultStr<RetargetAssetData> ParseRetarget( const std::string& text );

    /// Serialises a retarget back to the text `ParseRetarget` reads. Total over any @p data `Validate`
    /// accepts, and re-reads EQUAL — which is `operator==`, by value, not "the file is non-empty".
    NO_DISCARD std::string WriteRetarget( const RetargetAssetData& data );

    /// Reads and parses a `.retarget` from disk. The filesystem half, kept out of `ParseRetarget` so the
    /// format's rules stay testable without a file.
    NO_DISCARD Common::ResultStr<RetargetAssetData> LoadRetargetFile( const std::filesystem::path& path );

    /// Writes @p data to @p path atomically, reporting whether the BYTES ARRIVED — `SaveClipToFile`'s
    /// reason: an unchecked `operator<<` hands back a success value before the flush has happened.
    NO_DISCARD Common::BoolResultStr SaveRetargetFile( const std::filesystem::path& path,
                                                       const RetargetAssetData&     data );

    // ----------------------------------------------------------------------------------------------
    // File form -> runtime form
    // ----------------------------------------------------------------------------------------------

    /**
     * @brief The authored setup this file describes, in the form `Retargeter::Initialize` takes.
     *
     * Skeleton-free on purpose: the setup is still all names, so this step cannot fail for a reason that
     * needs a rig and the one that follows it owns every such refusal.
     */
    NO_DISCARD Common::ResultStr<Animation::Retarget::RetargetSetup>
               BuildRetargetSetup( const RetargetAssetData& data );

    // THERE IS DELIBERATELY NO `BuildRetargeter` HERE. Building one needs the SOURCE RIG KEPT, which is a
    // lifetime decision rather than a format one: `Animation::Retarget::RetargetSource::Create` owns it,
    // takes this setup and `SourceSkeletonSignature`, and is the ONE place a `Retargeter` is initialised.
    // A second initialiser in this layer would be a second answer to "is this the right rig", and the
    // engine would call one of them while the suites called the other.

    /// The exact mirror of `BuildRetargetSetup`, and it lives beside it for `BuildDataFromControlRig`'s
    /// reason: a format whose two directions are not testable together is a format whose round trip is an
    /// assumption.
    NO_DISCARD RetargetAssetData BuildDataFromRetargetSetup( const std::string& name,
                                                             const std::string& sourceSkeleton,
                                                             const Animation::Retarget::RetargetSetup& setup );
} // namespace Desert::Assets::Serialization
