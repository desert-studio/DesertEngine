#pragma once

// Extrude / Push-Pull / Inset / Outset on a selected region of an FDynamicMesh3, as UE's PolyEdit runs them on a
// copy of the mesh: FExtrudeOp (ModelingOperators/Private/DeformationOps/ExtrudeOp.cpp:94-146) drives
// FOffsetMeshRegion, UPolyEditInsetOutsetActivity (PolyEditInsetOutsetActivity.cpp:181) drives FInsetMeshRegion
// with the distance negated for Outset.
//
// TANGENTS. The ported region operations set normals and UVs on the triangles they create but never the tangent
// overlays: UE treats tangents as derived data and rebuilds them when the mesh description is built. A mesh that
// carries a tangent space here would otherwise leave the new triangles unset in it, and ToRenderMesh refuses such
// a mesh; so the result's tangents are recomputed from its normals and UV layer 0 (FMeshTangents, per triangle).

#include "Engine/Geometry/DynamicMeshSelection.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Geometry
{
    enum class RegionOperation : uint8_t
    {
        Extrude,
        PushPull,
        Inset,
        Outset,
    };

    [[nodiscard]] const char* ToString( RegionOperation operation );

    struct RegionOutcome
    {
        std::shared_ptr<const FDynamicMesh3> Mesh;
        // The moved region (Extrude, Push/Pull) or the inset region (Inset, Outset), in the selection's mode.
        ElementSelection Selection{ ElementMode::Triangle };
    };

    // Runs `operation` on a copy of `before`. `distance` is in cm: positive for Extrude / Inset / Outset, signed
    // for Push/Pull. Refused, by name and numbers, on a zero or wrongly signed distance, a selection covering no
    // whole triangle, the ported operation's own failure, or a tangent space with no UV layer 0 to derive from.
    [[nodiscard]] Common::ResultStr<RegionOutcome> RunRegionOperation( RegionOperation         operation,
                                                                       const FDynamicMesh3&    before,
                                                                       const ElementSelection& selection,
                                                                       float                   distance );
} // namespace Desert::Geometry
