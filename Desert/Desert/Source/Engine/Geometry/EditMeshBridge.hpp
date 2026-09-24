#pragma once

// THE TEMPORARY BRIDGE BETWEEN THE TWO MESH CORES (P8a). StaticMeshComponent::EditableMesh is an FDynamicMesh3;
// the modeling operations of M14-M17 and the tools that pick and select elements still run on EditMesh until
// their PORT0 cards (P10-P17) move them onto the ported core. Until then every crossing goes through THIS file
// and nothing else: outside Geometry/EditMesh* and this bridge no source includes an EditMesh header
// (EditMeshBridgeCensus pins it), so the day the last card lands, deleting this pair is the whole cleanup (P8b).
//
// The editor sees the EditMesh types (selection, operation arguments, the operations themselves) only through
// the includes below, which is why they are here and not in the callers.

#include "Engine/Geometry/EditMesh.hpp"
#include "Engine/Geometry/EditMeshConversion.hpp"
#include "Engine/Geometry/EditMeshModelOperations.hpp"
#include "Engine/Geometry/EditMeshNormals.hpp"
#include "Engine/Geometry/EditMeshOperations.hpp"
#include "Engine/Geometry/EditMeshSelection.hpp"
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
    // removed by P10 (GroupTopology + selection: picking and selection read the ported core).
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const EditMesh>>
    EditMeshView( const std::shared_ptr<const FDynamicMesh3>& mesh );

    // An operation's result back onto the ported core. The EditMesh is compacted first and @p selection (the
    // operation's output selection, if the caller keeps one) is renumbered through the same maps, so its IDs
    // name the returned mesh's elements. Refused when the converted mesh is refused by the FDynamicMesh3
    // reader. removed by the last of P11-P17 (each ports the operations that call it: P11 Extrude / Offset /
    // Inset, P13a/b Bevel, P14 Plane Cut + Mirror, P15 Subdivide, P17 Trim; the XForm tab and Create Shape
    // move with their own PORT0 cards).
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const FDynamicMesh3>>
    FromEditMesh( EditMesh mesh, ElementSelection* selection = nullptr );

    // FromEditMesh, then ECS::SetEditableMesh: what a tool that still BUILDS an EditMesh (Create Shape, the
    // CubeGrid blockout, a PolyEdit drag step) puts on the entity. Refused with either step's reason.
    // removed by P10 (PolyEdit) and by the PORT0 cards that port Create Shape and the CubeGrid bake.
    [[nodiscard]] Common::BoolResultStr SetEditableMeshFromEditMesh( ECS::StaticMeshComponent& component,
                                                                     EditMesh                  mesh );
} // namespace Desert::Geometry::Bridge
