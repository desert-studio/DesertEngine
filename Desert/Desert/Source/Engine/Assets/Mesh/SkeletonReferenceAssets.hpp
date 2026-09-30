#pragma once

#include <Engine/Animation/SkeletonReference.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <vector>

namespace Desert::Assets
{
    class SkinnedMeshAsset;
    class AnimationAsset;
    class SkeletonAsset;

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
} // namespace Desert::Assets

namespace Desert::Assets::Serialization
{
    /// Rewrites the asset's .skeleton: GUID, signature and bones are kept as the file states them; PreviewMesh and
    /// CompatibleSkeletons are taken from the asset (SetPreviewMesh / SetCompatibleSkeletons).
    [[nodiscard]] Common::BoolResultStr SaveSkeletonAsset( const SkeletonAsset& skeleton );

    /// Rewrites the `Skeleton` reference of the .skmesh at `skmeshPath`: a mesh source asset (MSAS) states it in
    /// its MeshSkin; an imported (cooked) .skmesh in its header AND in its raw source's import record
    /// (SetImportRecordSkeleton), so a re-import keeps the choice. Everything else in each file is kept.
    [[nodiscard]] Common::BoolResultStr SaveMeshSkeletonReference( const std::filesystem::path& skmeshPath,
                                                                   Common::Content::AssetGuid   skeleton );
} // namespace Desert::Assets::Serialization
