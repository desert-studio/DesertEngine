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

    // UE's EMeshVertexSculptBrushType, the ones this port carries so far.
    enum class SculptBrushType : uint8_t
    {
        Standard, // FSurfaceSculptBrushOp: push the surface along the brush normal (Ctrl / Invert pulls it in)
        Smooth,   // FSmoothBrushOp: pull each vertex toward its one-ring centroid
        Move,    // FMoveBrushOp: the vertices under the brush follow it, weighted by the falloff (Strength unused)
        Inflate, // FInflateBrushOp: each vertex pushed along its OWN vertex normal (Invert deflates)
        Flatten, // FFlattenBrushOp: each vertex pulled toward the plane fitted to the region under the brush
        Pinch,   // FPinchBrushOp: drawn toward the stroke line across the surface and sunk along the brush
                 // normal into a crease (Invert spreads and raises)
        SculptMax,  // FSurfaceMaxSculptBrushOp: Standard's push along the reference surface's normal, capped at
                    // MaxHeight * Radius from the surface the stroke started on (a plateau, not a spike)
        SmoothFill, // FSmoothFillBrushOp: Smooth that moves a vertex only where its one-ring centroid lies
                    // outside it - cavities fill in, ridges keep their height
        PlaneNormal,      // FPlaneBrushOp, UE "Plane (Normal)": toward the stroke plane through the first hit,
                          // across the region normal there
        PlaneViewAligned, // FPlaneBrushOp, UE "Plane (View)": the same plane, facing the view
        FixedPlane,       // FPlaneBrushOp on UE's work plane (UFixedPlaneBrushProperties): FixedPlanePosition /
                          // FixedPlaneRotation, set by T / Shift+T / Ctrl+Shift+T over the mesh or by value
        Erase, // UE's erase of the sculpt layer: back toward the mesh as it was when the sculpt session opened
    };
    // Every brush, in the order the panel's combo and the command palette list them (one list for both).
    inline constexpr SculptBrushType kSculptBrushTypes[] = {
        SculptBrushType::Standard,   SculptBrushType::Smooth,      SculptBrushType::Move,
        SculptBrushType::Inflate,    SculptBrushType::Flatten,     SculptBrushType::Pinch,
        SculptBrushType::SculptMax,  SculptBrushType::SmoothFill,  SculptBrushType::PlaneNormal,
        SculptBrushType::PlaneViewAligned, SculptBrushType::FixedPlane, SculptBrushType::Erase };

    // UE EPlaneBrushSideMode (UPlaneBrushOpProps::WhichPlaneSide): which vertices a plane brush moves.
    enum class SculptPlaneSide : uint8_t
    {
        BothSides,   // every vertex under the brush closes on the plane
        PushDown,    // only the vertices above the plane (on its normal's side) are pushed down to it
        PullTowards, // only the vertices below the plane are pulled up to it
    };
    inline constexpr SculptPlaneSide kSculptPlaneSides[] = { SculptPlaneSide::BothSides, SculptPlaneSide::PushDown,
                                                             SculptPlaneSide::PullTowards };
    [[nodiscard]] const char* ToString( SculptPlaneSide side );

    // The brushes FPlaneBrushOp drives (UE ESculptBrushOpTargetType::ActivePlane): their target is a plane the
    // stroke fixes at its start (SculptStrokeReference), not the surface under each stamp.
    [[nodiscard]] constexpr bool IsPlaneBrush( SculptBrushType type )
    {
        return type == SculptBrushType::PlaneNormal || type == SculptBrushType::PlaneViewAligned ||
               type == SculptBrushType::FixedPlane;
    }

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
        bool  Invert  = false; // Standard / Inflate / Pinch / Sculpt Max: the opposite move (UE: Ctrl held)
        // Sculpt Max (UE USculptMaxBrushOpProps::MaxHeight): the cap on the offset from the stroke's reference
        // surface, as a share of Radius.
        float MaxHeight = 0.5f;
        // Plane (Normal / View / Fixed) (UE UPlaneBrushOpProps::WhichPlaneSide).
        SculptPlaneSide PlaneSide = SculptPlaneSide::BothSides;
        // Plane (Fixed): UE's work plane (UFixedPlaneBrushProperties Position / Rotation), world cm and degrees
        // (pitch, yaw, roll about X, Y, Z, as the CubeGrid frame). The plane's normal is +Y turned by the rotation.
        glm::vec3 FixedPlanePosition{ 0.0f };
        glm::vec3 FixedPlaneRotation{ 0.0f };
    };

    // The Plane (Fixed) work plane's world normal: +Y turned by `rotationDegrees`, and the rotation that turns
    // +Y onto `normal` (what Shift+T writes back into FixedPlaneRotation).
    [[nodiscard]] glm::dvec3 FixedPlaneNormal( const glm::vec3& rotationDegrees );
    [[nodiscard]] glm::vec3  FixedPlaneRotationFor( const glm::dvec3& normal );

    // UE BaseBrushTool falloff: 1 inside (1 - Falloff) of the radius, then (1 - t^2)^3 to zero at the rim.
    [[nodiscard]] double SurfaceSculptFalloff( double distance, double radius, double falloff );

    // One stamp in world space: the hit point and the surface normal there, and PrevCenter, where the brush was
    // at the previous stamp of the stroke (UE FSculptBrushStamp's PrevStamp; equal to Center on the first one).
    // Move weighs the vertices by their distance to it and carries them by Center - PrevCenter; Pinch reads
    // the stroke direction from it, so the crease lies along the stroke.
    struct SurfaceSculptStamp
    {
        glm::dvec3 Center{ 0.0 };
        glm::dvec3 Normal{ 0.0, 1.0, 0.0 };
        glm::dvec3 PrevCenter{ 0.0 };
    };

    // Applies one stamp to `mesh` (placed by `localToWorld`). Returns the vertex ids it moved, and refreshes
    // the primary normal overlay's elements around them (seams stay seams: only element values change).
    // With `topology` (UE's Dynamic Mesh Sculpt, UDynamicMeshSculptTool) the edges under the brush are first
    // refined / collapsed toward topology->TargetEdgeLength (DynamicMeshRemesh RemeshRegion), so the stamp
    // moves a surface fine enough to carry it; without it the mesh keeps its triangles (Vertex Sculpt).
    std::vector<int> ApplySculptStamp( DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                       const SurfaceSculptStamp& stamp, const SculptBrushSettings& settings,
                                       const RemeshSettings* topology = nullptr );

    // The nearest triangle the world ray hits on `mesh` placed by `localToWorld`: the hit point (world) and the
    // triangle's world normal. False on a miss.
    [[nodiscard]] bool RaycastMesh( const DynamicMesh3& mesh, const glm::dmat4& localToWorld,
                                    const glm::dvec3& rayOrigin, const glm::dvec3& rayDirection,
                                    SurfaceSculptStamp& hitOut );
} // namespace Desert::Geometry
