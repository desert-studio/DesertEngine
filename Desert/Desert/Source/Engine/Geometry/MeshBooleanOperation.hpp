#pragma once

// Boolean (Union / Difference / Intersect) and Trim (Inside / Outside) of a mesh by a second mesh, as UE's
// CSG and Trim tools run FMeshBoolean on copies (UCSGMeshesTool / FBooleanMeshesOp, ModelingOperators
// BooleanMeshesOp.cpp): the ported MeshBoolean with its default tolerances, the cutter given in the target's
// space. NewGroupInside / NewGroupOutside regroup instead of deleting and are run the same way.

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/Operations/MeshBoolean.hpp"

#include <Common/Core/ResultStr.hpp>

#include <memory>

namespace Desert::Geometry
{
    using BooleanOperation = MeshBoolean::BooleanOp;

    [[nodiscard]] const char* ToString( BooleanOperation operation );

    struct BooleanOutcome
    {
        std::shared_ptr<const DynamicMesh3> Mesh;
    };

    // Runs `operation` on a copy of `target` with `cutter` (same space, cm). Refused, by name and numbers:
    //  - an empty target or cutter;
    //  - a cutter with open edges (inside/outside of it is undefined), and for Union / Difference / Intersect a
    //    target with open edges too (each mesh is classified against the other);
    //  - a Union / Difference / Intersect whose cut seam could not be welded shut (the count of open edges left);
    //  - an empty result (a disjoint Intersect, a Trim that removes everything);
    //  - a Trim that changes nothing (the cutter does not reach the mesh). NewGroup keeps every triangle, so a
    //    mesh wholly inside or outside the cutter is regrouped or left alone, and neither is refused.
    [[nodiscard]] Common::ResultStr<BooleanOutcome>
    RunMeshBoolean( BooleanOperation operation, const DynamicMesh3& target, const DynamicMesh3& cutter );
} // namespace Desert::Geometry
