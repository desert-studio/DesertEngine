#pragma once

#include <Engine/Animation/SkeletonReference.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace Desert::Assets
{
    class SkinnedMeshAsset;
    class AnimationAsset;
    class SkeletonAsset;
    class AssetManager;

    /**
     * @brief The asset side of the SKEL-TREE contract (Engine/Animation/SkeletonReference.hpp): what an asset
     * requires of a skeleton, and the writers that persist an authored skeleton reference.
     *
     * RequiredBonesOf returns exactly what Animation::CheckSkeletonAssignment takes:
     * - a mesh: every bone of its skin WITH its parent ("" = root), as UE USkeleton::IsCompatibleMesh;
     * - a clip: every track's bone name, parent not checked (a track names only its bone).
     */
    [[nodiscard]] std::vector<Animation::RequiredBone> RequiredBonesOf( const SkinnedMeshAsset& mesh );
    [[nodiscard]] std::vector<Animation::RequiredBone> RequiredBonesOf( const AnimationAsset& clip );

    /**
     * @brief THE ASSETS OF ONE SKELETON THAT NAME ITS BONES - what a Rename Bone is carried into (UE: Skeleton
     * Editing renames the bone in the skeleton and in the assets that reference it by name).
     *
     * `ClipFiles`: every .anim whose header names the skeleton (the registry's Rig tag, ReferrersOfSkeleton),
     * loaded or not. `RetargetFiles`: every .retarget whose SourceSkeleton or TargetSkeleton is this skeleton (the
     * file states both rigs of the pair by GUID, UE's IK Retargeter source/target IK Rig). `AnimGraphFiles` /
     * `ControlRigFiles`: every .danimgraph / .derig whose TargetSkeleton is this skeleton (UE
     * UAnimBlueprint::TargetSkeleton). `Loaded`: the manager whose resident AnimationAssets of the skeleton are
     * renamed in memory and whose resident RetargetAssets of a rewritten file re-read it; null = none resident.
     */
    struct SkeletonReferrers
    {
        std::vector<std::filesystem::path> ClipFiles;
        std::vector<std::filesystem::path> RetargetFiles;
        std::vector<std::filesystem::path> AnimGraphFiles;
        std::vector<std::filesystem::path> ControlRigFiles;
        AssetManager*                      Loaded = nullptr;
    };

    /// The referrers of @p skeleton as the content registry knows them, plus @p loaded's resident clips. Every
    /// registered retarget, anim graph and control rig is read for the skeleton GUIDs it states; one that does not
    /// read is refused by path (a referrer that cannot be checked is not silently left out of a rename).
    [[nodiscard]] Common::ResultStr<SkeletonReferrers>
    ReferrersOfSkeleton( const Common::Content::AssetGuid& skeleton, AssetManager* loaded );

    /**
     * @brief THE ONE POINT A BONE RENAME REACHES THE SKELETON'S ASSETS: every clip file of @p referrers is read,
     * its Bone bindings renamed (Timeline::RenameBoneLocators) and written back when one moved (SaveClipToFile
     * keeps its GUID and import record); every resident clip of @p skeleton in `Loaded` is renamed in memory
     * (AnimationAsset::RenameBones). Every retarget file whose SourceSkeleton is @p skeleton has its SOURCE-side
     * bone names renamed (RenameSourceBonesInRetarget) and is written back; a resident RetargetAsset of that file
     * re-reads it. A file that does not read or write is refused by path; the ones before it stay written. Called
     * by Serialization::SaveSkeletonAsset with the renames it reads off the file.
     */
    [[nodiscard]] Common::BoolResultStr
    RenameBonesInSkeletonAssets( const Common::Content::AssetGuid&                skeleton,
                                 std::span<const Animation::Timeline::BoneRename> renames,
                                 const SkeletonReferrers&                         referrers );
} // namespace Desert::Assets

namespace Desert::Assets::Serialization
{
    struct RetargetAssetData;

    /// RENAME BONE, IN A RETARGET WHOSE SOURCE RIG IS THE RENAMED SKELETON: every name the file states of the
    /// SOURCE rig (SourcePelvisBone, SourceRetargetPose offsets, chains' SourceStart/EndBone, BoneRenames'
    /// SourceBone) that is a `From` of @p renames gets its `To` - one simultaneous map, as RenameBoneLocators.
    /// The target side names the entity's own rig, which the file does not state, and is not touched. Returns
    /// how many names moved.
    std::size_t RenameSourceBonesInRetarget( RetargetAssetData&                               data,
                                             std::span<const Animation::Timeline::BoneRename> renames );

    /// Rewrites the asset's .skeleton: GUID and bone structure are kept as the file states them; every bone's NAME
    /// and LocalBindTransform, PreviewMesh and CompatibleSkeletons are taken from the asset. A bone whose name in
    /// memory is not the file's was RENAMED (SkeletonAsset::RenameBone; same index, the structure is the file's),
    /// and the renames read off that difference are carried into @p referrers (RenameBonesInSkeletonAssets) after
    /// the .skeleton is written. A file of another bone count is refused.
    [[nodiscard]] Common::BoolResultStr SaveSkeletonAsset( const SkeletonAsset&     skeleton,
                                                           const SkeletonReferrers& referrers );

    /// Rewrites the `Skeleton` reference of the .skmesh at `skmeshPath`: a mesh source asset (MSAS) states it in
    /// its MeshSkin; an imported (cooked) .skmesh in its header AND in its raw source's import record
    /// (SetImportRecordSkeleton), so a re-import keeps the choice. Everything else in each file is kept.
    [[nodiscard]] Common::BoolResultStr SaveMeshSkeletonReference( const std::filesystem::path& skmeshPath,
                                                                   Common::Content::AssetGuid   skeleton );
} // namespace Desert::Assets::Serialization
