#pragma once

// ISOTROPIC REMESHING ON A DynamicMesh3 — UE's FRemesher / FQueueRemesher (GeometryCore Remesher.h) as a
// pattern: every edge longer than 4/3 of the target is split, every edge shorter than 2/3 of it collapsed, edges
// flipped toward valence 6, vertices smoothed in their tangent plane and projected back onto the surface the
// mesh started as. Two callers, the way UE has two:
//   - RemeshMesh: the whole mesh (UE's Remesh tool, URemeshMeshTool, Target Edge Length);
//   - RemeshRegion: only under a brush, no smoothing (UE's Dynamic Mesh Sculpt tool, UDynamicMeshSculptTool,
//     which refines/collapses the edges its stamp touches so the surface gains resolution where it is sculpted).
//
// Lengths are WORLD centimetres measured through `localToWorld`, so a scaled object is remeshed at the edge
// length the user sees. Attribute seams (UV / normal) and open borders are kept: a seam or border vertex is
// never collapsed or smoothed and a seam edge never flipped (UE's MeshConstraints default for these tools);
// splitting one is allowed, the attribute set interpolates its elements.

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Geometry
{
    class DynamicMesh3;

    // UE URemeshProperties / the Dynamic Sculpt's remesh toggles.
    struct RemeshSettings
    {
        float TargetEdgeLength  = 5.0f;  // world cm
        bool  Splits            = true;  // UE bSplits ("Refine" in the sculpt tool)
        bool  Collapses         = true;  // UE bCollapses
        bool  Flips             = true;  // UE bFlips
        float SmoothingStrength = 0.25f; // UE SmoothingStrength (whole-mesh remesh only)
        int   Iterations        = 20;    // UE RemeshIterations
    };

    // Whole-mesh remesh in place; the primary normal overlay is recomputed afterwards. Returns false (mesh
    // untouched) for a non-positive target length or an empty mesh.
    bool RemeshMesh( DynamicMesh3& mesh, const glm::dmat4& localToWorld, const RemeshSettings& settings );

    // Split / collapse / flip of the edges whose midpoint lies within `radius` of `centreWorld`, repeated until
    // nothing changes (or `settings.Iterations` passes). Returns every vertex that was created or moved, plus the
    // endpoints of flipped edges, so the caller can refresh normals around them. No smoothing, no projection.
    std::vector<int> RemeshRegion( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                   const glm::dvec3& centreWorld, double radius, const RemeshSettings& settings );
} // namespace Desert::Geometry
