#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Geometry/EditMesh.hpp>

#include <span>

namespace Desert::Geometry
{
    // THE EDITMESH AS A STATIC MESH ASSET, AND BACK. The pair a modeling tool's "Output: Static Mesh" and a
    // later "edit this asset" stand on (UE: UE::Modeling::CreateMeshObject writes the dynamic mesh into a
    // UStaticMesh's MeshDescription, and the modeling tools lift it back out).
    //
    // WHAT A .stmesh CAN HOLD, AND WHAT IT CANNOT. The asset's vertex carries one normal, one tangent frame and
    // one UV, and each submesh one material; MeshBinary v2 adds one polygroup per face. That is exactly what
    // ToRenderMesh already puts on screen for an EditMesh entity, so the asset draws what the entity drew. A
    // mesh carrying MORE than that - a colour layer, or a second UV layer - is REFUSED by name rather than
    // written without it: the file would silently lose a layer the user authored.
    //
    // MATERIALS ARE SUBMESH SLOTS. ToRenderMesh emits one submesh per distinct MaterialID in ascending order,
    // and the renderer binds entity slot k to submesh k (not to MaterialID k). The asset keeps that rule:
    // submesh k's material is slotMaterials[k], and the mesh lifted back out has MaterialID k on submesh k's
    // triangles. A mesh whose IDs were already 0..N-1 comes back with the same IDs; one with gaps (0 and 2)
    // comes back compacted (0 and 1), bound to the same materials it drew with.

    // Refused, with the layer named, on a colour layer or more than one UV layer; otherwise refused only where
    // ToRenderMesh refuses (an unset normal / tangent / UV element). An empty mesh is refused: an asset with
    // no submesh draws nothing and has no bounds for the registry.
    [[nodiscard]] Common::ResultStr<Assets::Serialization::MeshAssetData>
    ToMeshAssetData( const EditMesh& mesh, std::span<const Common::Content::AssetGuid> slotMaterials );

    // The asset's triangles welded back into one EditMesh with the file's polygroups (all 0 for a file with
    // none), normals, tangents and UV as overlays, and MaterialID = submesh index. Refused on a skinned asset,
    // and on one whose triangles do not weld back one-to-one (degenerate, duplicate or non-manifold faces):
    // the per-face polygroups are addressed by face order, and a face the weld drops would shift every
    // group after it onto the wrong face.
    [[nodiscard]] Common::ResultStr<EditMesh>
    FromMeshAssetData( const Assets::Serialization::MeshAssetData& data );
} // namespace Desert::Geometry
