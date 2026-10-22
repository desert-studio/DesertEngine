#pragma once

// VERTEX SCULPTING ON A DynamicMesh3 — the brush half of UE's Modeling Mode Sculpt tool (UMeshVertexSculptTool,
// MeshSculptBrushOps.h / MeshSmoothingBrushOps.h, BaseBrushTool's falloff), ported as a pattern: a STAMP (where
// the brush touched the surface, its radius, power and falloff) applied to every vertex inside the radius. The
// tool (Editor/Panels/ViewportPanel/Tools/SculptTool) walks the stroke and lays the stamps; this file only moves
// vertices and refreshes the normals they bent, so it runs without a device or an editor.
//
// Radius and positions are WORLD centimetres: the stamp is placed through the entity's transform, so a scaled
// object is sculpted with the brush the user sees, not one shrunk by its scale.

#include "Engine/Geometry/DynamicMeshRemesh.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    class DynamicMesh3;

    // UE's EMeshVertexSculptBrushType, the two this port carries so far.
    enum class SculptBrushType : uint8_t
    {
        Standard, // FSurfaceSculptBrushOp: push the surface along the brush normal (Ctrl / Invert pulls it in)
        Smooth,   // FSmoothBrushOp: pull each vertex toward its one-ring centroid
    };

    [[nodiscard]] const char* ToString( SculptBrushType type );

    // UE's brush properties (UBrushBaseProperties + USculptBrushProperties): the brush size, its strength
    // and the share of the radius over which it fades.
    struct SculptBrushSettings
    {
        SculptBrushType Brush    = SculptBrushType::Standard;
        float           Radius   = 25.0f; // world cm
        float           Strength = 0.5f;  // 0..1, UE's BrushStrength
        float           Falloff  = 0.5f; // 0..1, UE's BrushFalloffAmount: 0 = hard edge, 1 = fades from the centre
        // UE's FlowRate/Spacing pair reduced to the spacing: a stamp every Spacing * Radius along the stroke.
        float Spacing = 0.25f;
        bool  Invert  = false; // Standard only: pull instead of push (UE: Ctrl held)
    };

    // UE BaseBrushTool falloff: 1 inside (1 - Falloff) of the radius, then (1 - t^2)^3 to zero at the rim.
    [[nodiscard]] double SculptFalloff( double distance, double radius, double falloff );

    // One stamp in world space: the hit point and the surface normal there.
    struct SculptStamp
    {
        glm::dvec3 Center{ 0.0 };
        glm::dvec3 Normal{ 0.0, 1.0, 0.0 };
    };

    // Applies one stamp to `mesh` (placed by `localToWorld`). Returns the vertex ids it moved, and refreshes
    // the primary normal overlay's elements around them (seams stay seams: only element values change).
    // With `topology` (UE's Dynamic Mesh Sculpt, UDynamicMeshSculptTool) the edges under the brush are first
    // refined / collapsed toward topology->TargetEdgeLength (DynamicMeshRemesh RemeshRegion), so the stamp
    // moves a surface fine enough to carry it; without it the mesh keeps its triangles (Vertex Sculpt).
    std::vector<int> ApplySculptStamp( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                       const SculptStamp& stamp, const SculptBrushSettings& settings,
                                       const RemeshSettings* topology = nullptr );

    // The nearest triangle the world ray hits on `mesh` placed by `localToWorld`: the hit point (world) and the
    // triangle's world normal. False on a miss.
    [[nodiscard]] bool RaycastMesh( const DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                    const glm::dvec3& rayOrigin, const glm::dvec3& rayDirection,
                                    SculptStamp& hitOut );
} // namespace Desert::Geometry
