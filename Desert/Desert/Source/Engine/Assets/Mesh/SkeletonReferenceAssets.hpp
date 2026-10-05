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
     * loaded or not. `Loaded`: the manager whose resident AnimationAssets of the skeleton are renamed in memory
     * beside their files; null = none resident.
     */
    struct SkeletonReferrers
    {
        std::vector<std::filesystem::path> ClipFiles;
        AssetManager*                      Loaded = nullptr;
    };

    /// The referrers of @p skeleton as the content registry knows them, plus @p loaded's resident clips.
    [[nodiscard]] SkeletonReferrers ReferrersOfSkeleton( const Common::Content::AssetGuid& skeleton,
                                                         AssetManager*                     loaded );

    /**
     * @brief THE ONE POINT A BONE RENAME REACHES THE SKELETON'S ASSETS: every clip file of @p referrers is read,
     * its Bone bindings renamed (Timeline::RenameBoneLocators) and written back when one moved (SaveClipToFile
     * keeps its GUID and import record); every resident clip of @p skeleton in `Loaded` is renamed in memory
     * (AnimationAsset::RenameBones). A clip that does not read or write is refused by path; the ones before it
     * stay written. Called by Serialization::SaveSkeletonAsset with the renames it reads off the file.
     */
    [[nodiscard]] Common::BoolResultStr
    RenameBonesInSkeletonAssets( const Common::Content::AssetGuid&                skeleton,
                                 std::span<const Animation::Timeline::BoneRename> renames,
                                 const SkeletonReferrers&                         referrers );
} // namespace Desert::Assets

namespace Desert::Assets::Serialization
{
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
