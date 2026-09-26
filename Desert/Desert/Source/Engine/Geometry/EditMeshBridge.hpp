#pragma once

// THE TEMPORARY BRIDGE BETWEEN THE TWO MESH CORES (P8a). StaticMeshComponent::EditableMesh is an FDynamicMesh3;
// the modeling operations of M14-M17 and the tools that pick and select elements still run on EditMesh until
// their PORT0 cards (P10-P17) move them onto the ported core. Until then every crossing goes through THIS file
// and nothing else: outside Geometry/EditMesh* and this bridge no source includes an EditMesh header
// (EditMeshBridgeCensus pins it), so the day the last card lands, deleting this pair is the whole cleanup (P8b).
//
// Which card removes which crossing: element selection and picking -> P10; Offset (ours, no UE counterpart) -> P11
// left it here; Edge Loop / Weld / Hole Fill / Clean -> P12; Bevel -> P13a/b; Plane Cut / Mirror -> P14; Subdivide
// -> P15; Create Shape and the shape generators -> P16; Boolean / Trim -> P17; the CubeGrid bake -> P18; the XForm
// tab (our own, no UE counterpart) -> P19 or a card of its own. The bridge as a whole -> P8b.
//
// The editor sees the EditMesh types (selection, operation arguments, the operations themselves) only through
// the includes below, which is why they are here and not in the callers.

#include "Engine/Geometry/EditMesh.hpp"
#include "Engine/Geometry/EditMeshAsset.hpp"
#include "Engine/Geometry/EditMeshConversion.hpp"
#include "Engine/Geometry/EditMeshModelOperations.hpp"
#include "Engine/Geometry/EditMeshNormals.hpp"
#include "Engine/Geometry/EditMeshOperations.hpp"
#include "Engine/Geometry/EditMeshSelection.hpp"
#include "Engine/Geometry/EditMeshSerialization.hpp"
#include "Engine/Geometry/EditMeshTopologyOperations.hpp"
#include "Engine/Geometry/EditMeshXformOperations.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <memory>

namespace Desert::ECS
{
    struct StaticMeshComponent;
}

namespace Desert::Geometry::Bridge
{
    // The EditMesh a tool reads for @p mesh: picking, element selection, the operations' input. IDs AGREE with
    // the FDynamicMesh3's: a mesh made by FromEditMesh is handed back the very EditMesh it was made from (so
    // edge IDs, which only the EditMesh has, stay those the selection was made on), and any other mesh is
    // compact (both cores' saved form numbers densely) and converted through the saved form, which keeps
    // vertex and triangle order. Cached per mesh for as long as the mesh lives - the component's mesh is
    // immutable, so a view never goes stale - because a pick runs on every mouse move.
    // Refused, naming the reason, when the saved form does not read back into an EditMesh.
    // removed by P10 (GroupTopology + selection: picking and selection read the ported core); the function
    // itself by P8b.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const EditMesh>>
    EditMeshView( const std::shared_ptr<const FDynamicMesh3>& mesh );

    // An operation's result back onto the ported core. The EditMesh is compacted first and @p selection (the
    // operation's output selection, if the caller keeps one) is renumbered through the same maps and its edges
    // are then carried onto the FDynamicMesh3's edge IDs by vertex pair, so its IDs name the returned mesh's
    // elements. Refused when the converted mesh is refused by the FDynamicMesh3
    // reader. removed by the last of the cards whose operations call it: Offset (ours; P11 ported Extrude /
    // Inset), P12 Edge Loop / Weld / Hole Fill / Clean, P13a/b Bevel, P14 Plane Cut / Mirror, P15 Subdivide, P17
    // Boolean / Trim, P19 (or its own card) the XForm tab; the function itself by P8b.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const FDynamicMesh3>>
    FromEditMesh( EditMesh mesh, ElementSelection* selection = nullptr );

    // @p selection (IDs of @p mesh) in the IDs of @p view = EditMeshView( mesh ), for an operation that still runs
    // on the EditMesh. Vertex, triangle and group IDs agree between the two (see EditMeshView); EDGE IDs are the
    // one kind the cores number differently, so an edge is carried over as its vertex pair. Refused, naming the
    // edge, when the view has no edge between those vertices (then @p view is not @p mesh's view). removed by
    // the same cards as FromEditMesh; the function itself by P8b.
    [[nodiscard]] Common::ResultStr<ElementSelection>
    ToEditMeshSelection( const FDynamicMesh3& mesh, const EditMesh& view, const ElementSelection& selection );

    // FromEditMesh, then ECS::SetEditableMesh: what a tool that still BUILDS an EditMesh (Create Shape, the
    // CubeGrid blockout, a PolyEdit drag step) puts on the entity. Refused with either step's reason.
    // removed by P10 (the PolyEdit drag), P16 (Create Shape and the shape generators) and P18 (the CubeGrid
    // bake); the function itself by P8b.
    [[nodiscard]] Common::BoolResultStr SetEditableMeshFromEditMesh( ECS::StaticMeshComponent& component,
                                                                     EditMesh                  mesh );

    // The mesh deriver's two crossings (AF4c): a MeshSourceAsset keeps its source in the saved form, and the
    // deriver builds on the EditMesh (tangents, the import transform) before writing the render buffers. Inline
    // so the deriver's suite, which compiles the geometry it runs and not the ECS behind the rest of this
    // bridge, links without it. Refused with FromSerialized's / ToMeshAssetData's reason. removed by the card
    // that moves MeshDeriver onto the ported core; the functions themselves by P8b.
    [[nodiscard]] inline Common::ResultStr<EditMesh> EditMeshFromSavedForm( const EditMeshSer& saved )
    {
        return FromSerialized( saved );
    }

    [[nodiscard]] inline Common::ResultStr<Assets::Serialization::MeshAssetData>
    MeshAssetDataFromEditMesh( const EditMesh& mesh, std::span<const Common::Content::AssetGuid> slotMaterials )
    {
        return ToMeshAssetData( mesh, slotMaterials );
    }

    // The mesh importer's two crossings (AF4d): an imported LOD arrives as render buffers, is lifted onto the
    // EditMesh to renumber its material ids by the shared slot table, and is stored as the saved form a
    // MeshSourceAsset keeps. Inline for the same reason as the deriver's pair: the importer's suite compiles
    // the geometry it runs, not the ECS behind the rest of this bridge. Refused with FromMeshAssetData's
    // reason. removed by the card that moves the importer onto the ported core; the functions themselves by P8b.
    [[nodiscard]] inline Common::ResultStr<EditMesh>
    EditMeshFromMeshAssetData( const Assets::Serialization::MeshAssetData& data )
    {
        return FromMeshAssetData( data );
    }

    [[nodiscard]] inline EditMeshSer SavedFormFromEditMesh( const EditMesh& mesh )
    {
        return ToSerialized( mesh );
    }
} // namespace Desert::Geometry::Bridge
