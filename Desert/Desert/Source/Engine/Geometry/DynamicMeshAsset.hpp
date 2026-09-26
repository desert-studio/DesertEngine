#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

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
    DynamicMeshToMeshAssetData( const DynamicMesh3&                         mesh,
                                std::span<const Common::Content::AssetGuid> slotMaterials );

    // The asset's triangles welded back (DynamicMeshFromRenderMesh) with triangle groups from the file (all 0
    // for a file with none) and MaterialID = submesh index. The same rule as the EditMesh reader
    // (FromMeshAssetData), so both cores open the same set of assets: as UE's MeshDescription -> DynamicMesh
    // conversion does, degenerate and duplicate faces are skipped and a third triangle on an edge stands on
    // its own corner copies; the counts come back with the mesh for the caller to say out loud, and each
    // surviving face keeps its own polygroup and bitangent sign through TriangleOfFace. Refused on a skinned
    // asset, and by the face count when no face survives the weld.
    [[nodiscard]] Common::ResultStr<ImportedDynamicMesh>
    DynamicMeshFromMeshAssetData( const Assets::Serialization::MeshAssetData& data );
} // namespace Desert::Geometry
