#pragma once

// ISOTROPIC REMESHING ON A DynamicMesh3 — UE's FRemesher / FQueueRemesher (GeometryCore Remesher.h) as a
// pattern, the ONE remesher of the editor: every edge longer than 4/3 of the target is split, every edge shorter
// than 2/3 of it collapsed, edges flipped toward valence 6, vertices smoothed in their tangent plane and projected
// onto a surface (UE FMeshProjectionTarget). Three callers, the way UE has three:
//   - RemeshMeshOperation: UE's Remesh tool (URemeshMeshTool / FRemeshMeshOp), the whole mesh, projected back
//     onto the surface it started as;
//   - the same with a projection target: UE's Project tool (UProjectToTargetTool, MeshProjectOperation.hpp);
//   - RemeshRegion: only under a brush, no smoothing (UE's Dynamic Mesh Sculpt tool, UDynamicMeshSculptTool,
//     which refines/collapses the edges its stamp touches so the surface gains resolution where it is sculpted).
//
// Lengths are measured through `localToWorld` (world centimetres for the Remesh tool and the sculpt, the source's
// object space for Project), so a scaled object is remeshed at the edge length the user sees. Open borders are
// kept; attribute seams (UV / normal) are kept while Preserve Sharp Edges is on (UE bPreserveSharpEdges: a seam
// vertex is never collapsed or smoothed, a seam edge never flipped); with it off a seam may collapse and smooth
// (UE bAllowSeamCollapse / bAllowSeamSmoothing = !bPreserveSharpEdges). Splitting a seam is always allowed, the
// attribute set interpolates its elements.

#include "Engine/Geometry/MeshDeformOperations.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    class DynamicMesh3;
    class ProgressCancel;

    // UE URemeshProperties (the pass switches) / the Dynamic Sculpt's remesh toggles.
    struct RemeshSettings
    {
        float TargetEdgeLength   = 5.0f;  // measured through localToWorld
        bool  Splits             = true;  // UE bSplits ("Refine" in the sculpt tool)
        bool  Collapses          = true;  // UE bCollapses
        bool  Flips              = true;  // UE bFlips
        float SmoothingStrength  = 0.25f; // UE SmoothingStrength (whole-mesh remesh only)
        int   Iterations         = 20;    // UE RemeshIterations
        bool  Reproject          = true;  // UE bReproject (whole-mesh remesh only)
        bool  PreserveSharpEdges = true;  // UE bPreserveSharpEdges: attribute seams stay

        bool operator==( const RemeshSettings& ) const = default;
    };

    // What the passes did, for the tool's report.
    struct RemeshCounts
    {
        int32_t Splits    = 0;
        int32_t Collapses = 0;
        int32_t Flips     = 0;
    };

    // UE 5.8 MeshModelingTools RemeshMeshTool.h, URemeshMeshToolProperties (defaults and ranges are UE's):
    //   Target Triangle Count (1..) or, with Use Target Edge Length, Target Edge Length (cm, > 0) - the edge
    //   length every edge is driven towards (the triangle count gives the equilateral edge that tiles the mesh's
    //   area with that many triangles, UE CalculateTargetEdgeLength);
    //   Smoothing Strength (0..1); Remesh Iterations (0..100); Flips / Splits / Collapses;
    //   Reproject (UE bReproject) - every pass ends on the projection surface;
    //   Preserve Sharp Edges (UE bPreserveSharpEdges) - attribute seams are constraints;
    //   Discard Attributes (UE bDiscardAttributes) - the UV / normal / colour overlays are dropped and per-vertex
    //   normals rebuilt, so the remesh is no longer held at their seams.
    // UE's Project tool shows the same properties (ProjectSettings is this struct).
    struct RemeshMeshSettings
    {
        int32_t TargetTriangleCount = 5000;
        bool    UseTargetEdgeLength = false;
        float   TargetEdgeLength    = 5.0f;
        float   SmoothingStrength   = 0.25f;
        int32_t RemeshIterations    = 20;
        bool    Flips               = true;
        bool    Splits              = true;
        bool    Collapses           = true;
        bool    Reproject           = true;
        bool    PreserveSharpEdges  = true;
        bool    DiscardAttributes   = false;

        bool operator==( const RemeshMeshSettings& ) const = default;
    };

    // UE FRemeshMeshOp: the input remeshed whole into a copy (DeformOutcome::Mesh, in the input's object space).
    // The projection surface is `projectTo` (already in the input's object space; UE UProjectToTargetTool's
    // ProjectionTarget) or, when null, the input itself (UE URemeshMeshTool). The normal overlay is recomputed
    // (per-vertex normals when the attributes were discarded). DeformOutcome::MovedVertices carries the result's
    // vertex count. Refused when a setting is out of its range, a mesh has no triangle, or the edge length would
    // ask for more than a million triangles. `toolName` heads the report and the refusals.
    [[nodiscard]] Common::ResultStr<DeformOutcome>
    RemeshMeshOperation( const DynamicMesh3& input, const glm::dmat4& localToWorld,
                         const RemeshMeshSettings& settings, const DynamicMesh3* projectTo = nullptr,
                         const char* toolName = "Remesh", const ProgressCancel* cancel = nullptr );

    // Split / collapse / flip of the edges whose midpoint lies within `radius` of `centreWorld`, repeated until
    // nothing changes (or `settings.Iterations` passes). Returns every vertex that was created or moved, plus the
    // endpoints of flipped edges, so the caller can refresh normals around them. No smoothing, no projection.
    std::vector<int> RemeshRegion( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                   const glm::dvec3& centreWorld, double radius, const RemeshSettings& settings );
} // namespace Desert::Geometry
