#pragma once

#include <Engine/Animation/Skeleton.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief THE SKELETON IS AN ASSET, AND EVERYTHING THAT NEEDS A RIG REFERENCES IT BY GUID (SKEL-TREE, owner
     * 09-30, variant "a", as UE).
     *
     * UE: USkeletalMesh::Skeleton and UAnimSequence::Skeleton are hard references to a USkeleton; a clip plays on
     * a mesh when both name ONE skeleton or the clip's skeleton is in the mesh skeleton's
     * USkeleton::CompatibleSkeletons; USkeleton::PreviewSkeletalMesh is the mesh the skeleton editor shows. Bone
     * names and the bone-structure hash are NOT identity there — they are the check run when a reference is
     * ASSIGNED (import, Details slot) and the retarget helper's input.
     *
     * Until now the identity here was the signature (a hash of the bones): AnimationAsset::m_SkeletonSignature,
     * MeshSourceAsset MeshSkin::SkeletonSignature, MeshAssetData::SkeletonSignature, ContentRegistry RigSignature,
     * ClipSkeletonMatch (signature OR majority of names), RetargetAsset SourceSkeletonSignature. A re-cooked rig
     * changed its hash and orphaned every mesh and clip that meant it; two exports of one rig (Mixamo with and
     * without end bones) were two identities. After SKEL-TREE:
     *
     * ONE HOME PER VALUE
     * - mesh -> skeleton:      SkinnedMeshAsset::GetSkeleton()  (.skmesh MeshAssetData: `Skeleton` AssetGuid,
     *                          MeshBinary version step; the source's MeshSkin carries the same GUID).
     * - clip -> skeleton:      AnimationAsset::GetSkeleton()    (.anim AnimationAssetData: `Skeleton`
     * AssetGuidRef, ANIM version step).
     * - skeleton -> preview:   SkeletonAsset::GetPreviewMesh()  (.skeleton SkeletonAssetData: `PreviewMesh`
     *                          AssetGuidRef, SKEL version step). Null = the skeleton editor shows bones only.
     * - skeleton -> compatible: SkeletonAsset::GetCompatibleSkeletons() (.skeleton `CompatibleSkeletons`
     *                          vector<AssetGuidRef>). ONE-DIRECTIONAL and NOT transitive: the MESH's skeleton
     *                          states which other skeletons' clips it accepts.
     * - "clip plays on mesh":  ClipPlaysOnMesh below — the one rule; picker, AnimGraph state resolution,
     *                          AnimationLibrary and Sequencer all ask it. ClipDrivesRig's name-majority half is
     *                          retired as a PLAYBACK rule (it survives only inside CheckSkeletonAssignment and
     *                          the retargeter, where names are the honest evidence).
     *
     * WHERE THE SIGNATURE STAYS (payload hash, never an identity)
     * - SkeletonAssetData::Signature / SkeletonAsset::GetSignature(): derived from the bones, kept on the asset.
     * - Import: FindSkeletonsBySignature pre-selects the existing skeleton an imported rig equals (the dialog's
     *   default); the artist may pick another, which must pass CheckSkeletonAssignment. No match = a new
     *   .skeleton is written and referenced.
     * - Migration: MigrateSkeletonReference, once per legacy file; after it no file stores a signature as a
     *   reference (AnimationAssetData / MeshAssetData / MeshSkin / RetargetAssetData lose the field).
     *
     * MIGRATION (no legacy readers stay behind)
     * - Tools/SceneMigrator raises every .anim / .skmesh / source / .retarget: legacy signature -> the one
     *   .skeleton with that signature -> its GUID. Zero or several candidates is a refusal naming the file and
     *   every candidate path; the migrator stops, nothing is guessed. The readers then refuse the old
     *   generation by name (RefuseTextWithoutHeader / MeshBinary version), as every other format step does.
     */

    /// One side of the playback rule: the referenced skeleton's GUID (identity) and a name for refusals (its
    /// path as the referencing asset states it). A null GUID means "no skeleton referenced".
    struct SkeletonAssetRef
    {
        Common::Content::AssetGuid Guid;
        std::string                Name;
    };

    /**
     * @brief THE RULE: does a clip whose skeleton is `clipSkeleton` play on a mesh whose skeleton is
     *        `meshSkeleton`, given that skeleton's CompatibleSkeletons?
     *
     * Success iff both GUIDs are non-null and either equal or `clipSkeleton.Guid` is listed in
     * `meshSkeletonCompatible`. Otherwise a refusal that names BOTH skeletons (clip's and mesh's `Name`) and
     * says which condition failed (clip names no skeleton / mesh names no skeleton / different and not
     * compatible). Pure: no asset manager, no bones, no signature.
     */
    [[nodiscard]] Common::BoolResultStr
    ClipPlaysOnMesh( const SkeletonAssetRef& clipSkeleton, const SkeletonAssetRef& meshSkeleton,
                     std::span<const Common::Content::AssetGuid> meshSkeletonCompatible );

    /// A bone the assigned asset needs from the skeleton. `Parent` = nullopt: parent not checked (a clip track
    /// names only its bone); "" = must be a root; otherwise the skeleton's parent of `Name` must be `Parent`
    /// (a mesh's skin hierarchy, as UE USkeleton::IsCompatibleMesh).
    struct RequiredBone
    {
        std::string                Name;
        std::optional<std::string> Parent;
    };

    /**
     * @brief THE ASSIGNMENT CHECK — the one place bone names decide anything about a skeleton reference.
     *
     * Run when a reference is SET (import dialog, Details slot of a mesh or clip, migration verification), never
     * at playback. Success iff every required bone exists on `skeleton` by name and, where `Parent` is stated,
     * has that parent. A refusal names the asset, the skeleton and EVERY missing or mis-parented bone.
     */
    [[nodiscard]] Common::BoolResultStr CheckSkeletonAssignment( const Skeleton& skeleton, std::string_view skeletonName,
                                                                 std::span<const RequiredBone> required,
                                                                 std::string_view              assetName );

    /// What migration and import may know about one registered skeleton: GUID, payload hash, path (for refusals).
    struct SkeletonCandidate
    {
        Common::Content::AssetGuid Guid;
        uint64_t                   Signature = 0;
        std::string                Path;
    };

    /// Indices into `skeletons` of every candidate whose Signature equals `signature` (non-zero), in input order.
    /// Signature 0 ("never read" / "no rig claimed") matches nothing.
    [[nodiscard]] std::vector<size_t> FindSkeletonsBySignature( uint64_t                           signature,
                                                                std::span<const SkeletonCandidate> skeletons );

    /**
     * @brief MIGRATION: the GUID a legacy file's signature reference becomes.
     *
     * Exactly one candidate with `legacySignature` -> its GUID. Otherwise a refusal that starts with
     * `referencingPath` and states: signature 0 ("names no skeleton"), no candidate among N, or the AMBIGUITY
     * with every matching candidate's Path. Never picks the first of several, never mints a skeleton.
     */
    [[nodiscard]] Common::ResultStr<Common::Content::AssetGuid>
    MigrateSkeletonReference( std::string_view referencingPath, uint64_t legacySignature,
                              std::span<const SkeletonCandidate> skeletons );
} // namespace Desert::Animation
