#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <functional>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    // FDYNAMICMESH3 AS A STATIC MESH ASSET, AND BACK. UE's counterpart (UE::Modeling::CreateMeshObject writing the
    // dynamic mesh into a UStaticMesh's FMeshDescription) stands on FMeshDescription and UObject, neither of which
    // Desert has, so this is not a port; the file side is MeshAssetArrays.hpp, so the file depends only on the
    // render arrays the mesh produces.
    //
    // WHAT CROSSES. What DynamicMeshRenderConversion.hpp carries (primary normals, tangent + bitangent
    // overlays, one UV layer, MaterialID), plus the mesh's triangle groups as MeshBinary v2's per-face
    // PolyGroups (a mesh without triangle groups writes group 0 on every face). A mesh
    // carrying more than a .stmesh holds - a colour overlay, a second UV layer, an extended polygroup layer -
    // is REFUSED by name rather than written without it. LODs are not written: they are derived at cook.
    //
    // MATERIALS ARE SUBMESH SLOTS: one submesh per distinct MaterialID ascending,
    // submesh k takes slotMaterials[k], and the mesh read back has MaterialID k on submesh k's triangles
    // (IDs with gaps come back compacted, bound to the same materials).

    // Refused on an empty mesh, a colour overlay, more than one UV layer or a polygroup layer; otherwise
    // only where ToRenderMesh refuses (no normal overlay, an unset element in a carried overlay).
    [[nodiscard]] Common::ResultStr<Assets::Serialization::MeshAssetData>
    DynamicMeshToMeshAssetData( const DynamicMesh3&                         mesh,
                                std::span<const Common::Content::AssetGuid> slotMaterials );

    // The asset's triangles welded back (DynamicMeshFromRenderMesh) with triangle groups from the file (all 0
    // for a file with none) and MaterialID = submesh index. As UE's MeshDescription -> DynamicMesh
    // conversion does, degenerate and duplicate faces are skipped and a third triangle on an edge stands on
    // its own corner copies; the counts come back with the mesh for the caller to say out loud, and each
    // surviving face keeps its own polygroup and bitangent sign through TriangleOfFace. Refused on a skinned
    // asset, and by the face count when no face survives the weld.
    [[nodiscard]] Common::ResultStr<ImportedDynamicMesh>
    DynamicMeshFromMeshAssetData( const Assets::Serialization::MeshAssetData& data );

    // THE MATERIAL IDS OF A MESH, for a caller that rebinds them to a slot table without reaching into the
    // attribute set (the editor goes through these adapters, scripts/CI/MeshCoreIncludes.sh).
    // The highest MaterialID a live triangle carries; -1 for a mesh with no MaterialID attribute or no triangle.
    [[nodiscard]] int MaxMaterialId( const DynamicMesh3& mesh );
    // Every triangle's MaterialID m becomes newIdOfOld[m]. A mesh with no MaterialID attribute is left as it is;
    // refused, naming the triangle and the ID, when an ID falls outside the table (nothing is changed then).
    [[nodiscard]] Common::BoolResultStr RemapMaterialIds( DynamicMesh3& mesh, std::span<const int> newIdOfOld );
    // The distinct MaterialIDs the live triangles carry, ascending; {0} for a mesh with no MaterialID attribute
    // (every triangle is material 0 then, as ToRenderMesh reads it).
    [[nodiscard]] std::vector<int> DistinctMaterialIds( const DynamicMesh3& mesh );
    // Every triangle's MaterialID m becomes map(m), in triangle order (so a map that grows a table sees first
    // use first). A mesh with an attribute set but no MaterialID attribute gains one first (all 0); refused on a
    // mesh with no attribute set.
    [[nodiscard]] Common::BoolResultStr MapMaterialIds( DynamicMesh3& mesh, const std::function<int( int )>& map );
} // namespace Desert::Geometry
