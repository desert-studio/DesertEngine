#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/DynamicMeshRemesh.hpp"
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Geometry
{
    // UE 5.8 MeshModelingTools ProjectToTargetTool.h: the Project tool shows URemeshMeshToolProperties, the Remesh
    // tool's own properties (DynamicMeshRemesh.hpp); Reproject is its projection onto the target.
    using ProjectSettings = RemeshMeshSettings;

    // UE UProjectToTargetTool (FRemeshMeshOp with a ProjectionTarget): the source mesh is remeshed towards the
    // target edge length and, pass by pass, projected onto the target mesh's surface (its nearest point, through
    // the target's AABB tree), so it takes the target's shape with an even triangulation of its own. The target is
    // brought into the source's object space by targetToSource (UE transforms the target into the tool mesh's
    // frame the same way); the result is in the source's object space. The remesh is the Remesh tool's
    // (RemeshMeshOperation with the target as its projection surface, lengths in the source's object space).
    // DeformOutcome::MovedVertices carries the result's vertex count. Refused when a setting is out of its range,
    // a mesh has no triangle, or the target edge length would ask for more than a million triangles.
    [[nodiscard]] Common::ResultStr<DeformOutcome> ProjectToTarget( const DynamicMesh3&    source,
                                                                    const DynamicMesh3&    target,
                                                                    const glm::dmat4&      targetToSource,
                                                                    const ProjectSettings& settings );
} // namespace Desert::Geometry
