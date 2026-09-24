#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp>

#include <span>

namespace Desert::Geometry
{
    // FDYNAMICMESH3 AS A STATIC MESH ASSET, AND BACK: EditMeshAsset.hpp carried onto the ported core, with the
    // same rules and the same file. UE's counterpart (UE::Modeling::CreateMeshObject writing the dynamic mesh
    // into a UStaticMesh's FMeshDescription) stands on FMeshDescription and UObject, neither of which Desert
    // has, so this is not a port; the file side is MeshAssetArrays.hpp, shared with the EditMesh pair, so a
    // mesh both cores draw the same way is written to the same bytes.
    //
    // WHAT CROSSES. What DynamicMeshRenderConversion.hpp carries (primary normals, tangent + bitangent
    // overlays, one UV layer, MaterialID), plus the mesh's triangle groups as MeshBinary v2's per-face
    // PolyGroups (a mesh without triangle groups writes group 0 on every face, as an EditMesh does). A mesh
    // carrying more than a .stmesh holds - a colour overlay, a second UV layer, an extended polygroup layer -
    // is REFUSED by name rather than written without it. LODs are not written: they are derived at cook.
    //
    // MATERIALS ARE SUBMESH SLOTS, as in EditMeshAsset.hpp: one submesh per distinct MaterialID ascending,
    // submesh k takes slotMaterials[k], and the mesh read back has MaterialID k on submesh k's triangles
    // (IDs with gaps come back compacted, bound to the same materials).

    // Refused on an empty mesh, a colour overlay, more than one UV layer or a polygroup layer; otherwise
    // only where ToRenderMesh refuses (no normal overlay, an unset element in a carried overlay).
    [[nodiscard]] Common::ResultStr<Assets::Serialization::MeshAssetData>
    DynamicMeshToMeshAssetData( const FDynamicMesh3&                        mesh,
                                std::span<const Common::Content::AssetGuid> slotMaterials );

    // The asset's triangles welded back (DynamicMeshFromRenderMesh) with triangle groups from the file (all 0
    // for a file with none) and MaterialID = submesh index; triangle k is face k of the file. Refused on a
    // skinned asset and on faces that do not weld back one-to-one (degenerate, duplicate, or a third triangle
    // on an edge): the per-face polygroups are addressed by face order, and the EditMesh reader refuses the
    // same files, so both cores open exactly the same set of assets.
    [[nodiscard]] Common::ResultStr<FDynamicMesh3>
    DynamicMeshFromMeshAssetData( const Assets::Serialization::MeshAssetData& data );
} // namespace Desert::Geometry
