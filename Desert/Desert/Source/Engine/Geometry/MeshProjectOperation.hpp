#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Geometry
{
    // UE 5.8 MeshModelingTools ProjectToTargetTool.h / RemeshMeshTool.h (URemeshMeshToolProperties, the subset the
    // Project tool shows; defaults and ranges are UE's):
    //   Target Triangle Count (1..) or, with Use Target Edge Length, Target Edge Length (cm, > 0) - the edge
    //   length the remesh drives every edge towards (collapse below 0.66x, split above 1.33x, UE FRemesher);
    //   Smoothing Strength (0..1) - the uniform one-ring smoothing step of every pass;
    //   Remesh Iterations (0..100) - the refine / smooth / project passes;
    //   Flips / Splits / Collapses - which edge edits the passes may make;
    //   Projection (UE bReproject) - every pass ends by moving each vertex onto the target surface;
    //   Discard Attributes - the UV / normal / colour overlays are dropped and per-vertex normals rebuilt, so the
    //   remesh is no longer held at their seams.
    struct ProjectSettings
    {
        int32_t TargetTriangleCount = 5000;
        bool    UseTargetEdgeLength = false;
        float   TargetEdgeLength    = 5.0f;
        float   SmoothingStrength   = 0.25f;
        int32_t RemeshIterations    = 20;
        bool    Flips               = true;
        bool    Splits              = true;
        bool    Collapses           = true;
        bool    Projection          = true;
        bool    DiscardAttributes   = false;

        bool operator==( const ProjectSettings& ) const = default;
    };

    // UE UProjectToTargetTool (FRemeshMeshOp with a ProjectionTarget): the source mesh is remeshed towards the
    // target edge length and, pass by pass, projected onto the target mesh's surface (its nearest point, through
    // the target's AABB tree), so it takes the target's shape with an even triangulation of its own. The target is
    // brought into the source's object space by targetToSource (UE transforms the target into the tool mesh's
    // frame the same way); the result is in the source's object space. Boundary and attribute-seam edges are kept
    // (split, never collapsed or flipped; their vertices are not smoothed), as UE's seam constraints do; the
    // normal overlay is recomputed. DeformOutcome::MovedVertices carries the result's vertex count. Refused when a
    // setting is out of its range, a mesh has no triangle, or the target edge length would ask for more than a
    // million triangles.
    [[nodiscard]] Common::ResultStr<DeformOutcome> ProjectToTarget( const DynamicMesh3&    source,
                                                                    const DynamicMesh3&    target,
                                                                    const glm::dmat4&      targetToSource,
                                                                    const ProjectSettings& settings );
} // namespace Desert::Geometry
