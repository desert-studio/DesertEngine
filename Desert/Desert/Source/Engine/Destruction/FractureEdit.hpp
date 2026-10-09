#pragma once

// THE FRACTURE MODE'S COMMAND LAYER (DST-02): what the editor's Fracture mode does to a `.dfrac`, below the
// editor so a suite can run it without a window (UE: FractureEditorMode's tools act on a UGeometryCollection
// through FFractureEngineFracturing; the toolkit is only their UI).
//
// THREE KINDS OF STATE, KEPT APART BECAUSE UE KEEPS THEM APART:
//   * the FRACTURE (FractureData): pieces, levels, interior material — saved in the asset, one undo step per
//     Generate or interior-material change (Assets::FractureAsset::WriteStep);
//   * the GENERATE SETTINGS (FractureSettings): saved with the fracture, so a re-bake reproduces the file;
//   * the VIEW (FractureViewSettings): UE's Explode Amount and Fracture Level live in UFractureSettings, an
//     editor tool-settings object, and UGeometryCollectionComponent::ApplyExplodedView moves only the
//     component's display transforms — never the collection's rest transforms. Ours is the same: preview
//     state, not serialized (census FractureMode.TheExplodeSliderIsPreviewStateNotSerialized).
//
// Every number is in centimetres.

#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Destruction
{
    /// UE UFractureSettings::ExplodeAmount / FractureLevel: how far the preview pushes pieces apart, and
    /// which level it pushes. Never written to the `.dfrac`.
    struct FractureViewSettings
    {
        float   ExplodeAmount = 0.0f; // 0 = assembled, 1 = each piece moved by its own offset from its parent
        int32_t ViewLevel     = -1;   // -1 = every level (UE "All Levels"); N = only level N separates
    };

    /// The preview offset of every node (cm, in the fracture's space), parallel to @p nodes. UE
    /// ApplyExplodedView: with every level shown, a node moves by its parent's offset plus (its centre - its
    /// parent's centre) * amount, so deeper levels spread inside their parent; with one level shown, the nodes
    /// of that level move by (their centre - the root's centre) * amount and their subtree moves with them.
    [[nodiscard]] std::vector<glm::dvec3> ExplodedOffsets( const std::vector<FractureNode>& nodes,
                                                           const FractureViewSettings&      view );

    /// Generate (UE's Fracture button): bakes @p source with @p settings into the fracture that replaces
    /// @p current. The fracture records @p sourceMesh, the GUID of the static mesh asset @p source was read
    /// from, as its source - on the first Generate of a new `.dfrac` as on a re-bake of another mesh (UE: the
    /// collection remembers the mesh it was made from). The identity (GUID) and the interior material are
    /// @p current's: a re-bake changes the pieces, never what the asset is or what its interior draws with.
    [[nodiscard]] Common::ResultStr<FractureData> GenerateFracture( const Geometry::DynamicMesh3&     source,
                                                                    const Common::Content::AssetGuid& sourceMesh,
                                                                    const FractureData&               current,
                                                                    const FractureSettings&           settings );

    /// The deepest level the fracture has (0 for an unfractured root): the range of the view's level list.
    [[nodiscard]] uint32_t DeepestLevel( const std::vector<FractureNode>& nodes );
} // namespace Desert::Destruction
