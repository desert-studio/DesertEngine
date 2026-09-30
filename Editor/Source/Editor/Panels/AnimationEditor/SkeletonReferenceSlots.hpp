#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
    class AnimationAsset;
    class SkeletonAsset;
    class SkinnedMeshAsset;
} // namespace Desert::Assets

namespace Desert::Editor::SkeletonSlots
{
    /**
     * @brief THE SKELETON REFERENCES AS DETAILS SLOTS (SKEL-TREE; contract Engine/Animation/SkeletonReference.hpp).
     *
     * UE's SPropertyEditorAsset for USkeletalMesh::Skeleton, UAnimSequence::Skeleton, USkeleton::PreviewSkeletalMesh
     * and USkeleton::CompatibleSkeletons: a sunk field naming the referenced asset, a picker listing the registry's
     * rows of the kind (nothing is loaded to list them), Open / Browse beside it. A reference is a GUID; its name
     * is the registry row's file name. Assigning a SKELETON to a mesh or a clip passes CheckSkeletonAssignment
     * first — a refusal lists every missing or mis-parented bone and nothing is written.
     */

    /// The registry's file name of the asset a GUID names, "None" for a null GUID, "<guid> (not registered)"
    /// when no row of `kind` answers for it.
    [[nodiscard]] std::string NameOf( Common::Content::ContentKind kind, const Common::Content::AssetGuid& guid );

    /// The asset handle a GUID folds to (0 for a null GUID): what Open / Browse and the asset manager take.
    [[nodiscard]] uint64_t HandleOf( const Common::Content::AssetGuid& guid );

    /// One slot. `accept` narrows the picker's registry rows (null = every row of `kind`); `allowNone` offers "None".
    /// Returns the GUID picked this frame. The picker's search text lives in `filter` (the caller's, per slot).
    [[nodiscard]] std::optional<Common::Content::AssetGuid>
    DrawGuidSlot( const char* id, Common::Content::ContentKind kind, const Common::Content::AssetGuid& current,
                  bool allowNone, std::array<char, 64>& filter,
                  const std::function<bool( const Assets::ContentRegistry::PickerRow& )>& accept = {} );

    /// Find-or-create the `.skeleton` a GUID names and load it; a refusal names the GUID or the file.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<Assets::SkeletonAsset>>
    LoadSkeleton( Assets::AssetManager& assets, const Common::Content::AssetGuid& skeleton );

    /// UE USkeletalMesh::Skeleton assignment: CheckSkeletonAssignment over the mesh's skin bones (with parents),
    /// then SetSkeleton and the `.skmesh` rewritten (SaveMeshSkeletonReference). Nothing changes on a refusal.
    [[nodiscard]] Common::BoolResultStr AssignMeshSkeleton( Assets::AssetManager& assets, Assets::SkinnedMeshAsset& mesh,
                                                            const Common::Content::AssetGuid& skeleton );

    /// UE UAnimSequence::Skeleton assignment: CheckSkeletonAssignment over the clip's track bones, then
    /// SetSkeleton. The caller saves the clip (the `.anim` is the Animation Editor's document).
    [[nodiscard]] Common::BoolResultStr AssignClipSkeleton( Assets::AssetManager& assets, Assets::AnimationAsset& clip,
                                                            const Common::Content::AssetGuid& skeleton );
} // namespace Desert::Editor::SkeletonSlots
