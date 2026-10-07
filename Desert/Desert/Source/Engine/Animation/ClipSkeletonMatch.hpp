#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Animation/SkeletonReference.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief THE PICKER AND THE RUNTIME ASK ONE RULE: ClipPlaysOnMesh (SkeletonReference.hpp).
     *
     * There used to be two answers (signature equality at runtime, a name majority in the pickers), then their
     * union (ClipDrivesRig). Since SKEL-TREE the skeleton is an asset and clips and meshes reference it by GUID,
     * so "does this clip play here" is a comparison of references plus the mesh skeleton's CompatibleSkeletons.
     * Bone names decide nothing here any more; they are checked once, when a reference is ASSIGNED
     * (CheckSkeletonAssignment). `Desert/Tests/Engine/ClipSkeletonMatch` asserts the picker and the runtime agree.
     */

    /// Everything the rule looks at on the clip side. The library records this once, at Register.
    struct ClipRigIdentity
    {
        Common::AssetHandle Handle;
        std::string         ClipName;

        /// AnimationAsset::GetSkeleton() and the clip's name for refusals. Null GUID = the clip names no skeleton
        /// and plays nowhere.
        SkeletonAssetRef Skeleton;
    };

    /// The mesh side: its skeleton reference (SkinnedMeshAsset::GetSkeleton()) and that skeleton's
    /// CompatibleSkeletons (SkeletonAsset::GetCompatibleSkeletons()). Built by the caller that owns the assets.
    struct MeshSkeletonIdentity
    {
        SkeletonAssetRef                        Skeleton;
        std::vector<Common::Content::AssetGuid> Compatible;
    };

    /// The PICKER side: every clip that plays on this mesh, as indices into `clips`.
    [[nodiscard]] std::vector<size_t> SelectClipsForMesh( const std::vector<ClipRigIdentity>& clips,
                                                          const MeshSkeletonIdentity&         mesh );

    /**
     * @brief The RUNTIME side: the one clip a state names. Same rule, and a NAMED refusal when there is none:
     *        either no such clip is registered, or it is and ClipPlaysOnMesh refused it (its reason, which
     *        names both skeletons, is carried through).
     */
    [[nodiscard]] Common::ResultStr<size_t> FindClipForMesh( const std::vector<ClipRigIdentity>& clips,
                                                             const MeshSkeletonIdentity&         mesh,
                                                             const std::string&                  clipName );
} // namespace Desert::Animation
