#pragma once

// A RIVER water body (UE 5.x AWaterBodyRiver / UWaterBodyRiverComponent): a ribbon of water along a spline,
// RiverWidth wide, flowing from the first point to the last. Its surface is drawn by the same water zone quadtree
// as the ocean (WaterQuadTree tags each tile with the body that owns it, a river over an ocean); the FLOW FIELD
// baked here from the spline's tangent (UE bakes the river's velocity into the WaterInfo texture,
// WaterInfoRendering .cpp) is what the surface's two-phase flow advects its normals and foam with.
//
// The points are relative to the body entity's location (the entity carries the river as the ocean entity carries
// its footprint). Pure CPU, no renderer types: the collector bakes, the renderer uploads.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Water
{
    /// What a water body is (ECS::WaterBodyData::Type). UE: AWaterBodyOcean / AWaterBodyRiver.
    enum class WaterBodyType : uint8_t
    {
        Ocean,
        River,
    };

    /// The tiles' body precedence: where a river's footprint overlaps an ocean's the river's surface is drawn.
    [[nodiscard]] constexpr int32_t WaterBodyPriority( const WaterBodyType type ) noexcept
    {
        return type == WaterBodyType::River ? 1 : 0;
    }

    /// A uniform Catmull-Rom spline through @p points, the ends clamped (the first and last
    /// segments use their own end point as the missing neighbour). @p t in [0, points.size() - 1].
    [[nodiscard]] glm::vec3 RiverSplinePoint( std::span<const glm::vec3> points, float t ) noexcept;
    /// d/dt of RiverSplinePoint (not normalised; zero only on a degenerate spline).
    [[nodiscard]] glm::vec3 RiverSplineTangent( std::span<const glm::vec3> points, float t ) noexcept;

    /// The river's footprint on the XZ plane (cm, the spline's frame): the swept ribbon's bounds, half the width
    /// out from every sampled point. Min > Max (an empty box) for fewer than two points or a non-positive width.
    struct RiverFootprint
    {
        glm::vec2 Min{ 0.0f };
        glm::vec2 Max{ 0.0f };
    };
    [[nodiscard]] RiverFootprint ComputeRiverFootprint( std::span<const glm::vec3> points, float width ) noexcept;

    /// The baked flow of one river over its footprint (UE: the river velocity in the WaterInfo texture), row-major
    /// over (z, x), texel centres at Min + (i + 0.5) * Extent / Size. Each texel:
    ///   xy = the surface velocity on the XZ plane, cm/s (the spline's unit tangent * FlowSpeed, eased to 0 at the
    ///        banks so the banks do not shear);
    ///   z  = the bank mask: 1 inside the ribbon, 0 outside (the surface is clipped where it is 0);
    ///   w  = the distance along the spline, cm (the flow phase's anchor; foam streaks follow it).
    struct RiverFlowField
    {
        glm::vec2              Min{ 0.0f };
        glm::vec2              Extent{ 0.0f };
        glm::uvec2             Size{ 0u };
        std::vector<glm::vec4> Texels;
        /// The two-phase flow's period (s): ECS::WaterBodyData::RiverFlowPeriod.
        float FlowPeriod = 1.0f;
        /// The spline's mean height, cm: the level the quadtree picks a river tile's LOD at (the surface itself
        /// follows SurfaceHeightCm).
        float SurfaceY = 0.0f;
        /// The still surface's height per texel, cm (UE: the river spline's height under the point - the water
        /// level falls along the flow): the nearest spline point's Y inside the banks, 0 outside them (read through
        /// the bank mask, SampleRiverSurfaceY). Texels.size() entries, the same layout.
        std::vector<float> SurfaceHeightCm;
        /// The lowest and highest SurfaceHeightCm inside the banks (the tiles' vertical bounds), cm.
        float MinSurfaceY = 0.0f;
        float MaxSurfaceY = 0.0f;
    };

    /// The largest side a flow field is baked at (texels): a longer river gets coarser texels, not a bigger image.
    inline constexpr uint32_t kRiverFlowMaxSize = 1024;

    /// Bakes @p points (already in world space) into a flow field of about @p cmPerTexel (clamped so neither side
    /// exceeds kRiverFlowMaxSize). An empty field (Size 0) for fewer than two points or a non-positive width.
    /// The field's texel nearest @p xz (world XZ, cm); zero outside the field or for an empty one.
    [[nodiscard]] glm::vec4 SampleRiverFlow( const RiverFlowField& field, glm::vec2 xz ) noexcept;

    /// The still surface's height at @p xz (world XZ, cm): SurfaceHeightCm filtered bilinearly over the texels
    /// inside the banks only (a bank texel's 0 never drags the level down); nullopt outside the banks or the field.
    [[nodiscard]] std::optional<float> SampleRiverSurfaceY( const RiverFlowField& field, glm::vec2 xz ) noexcept;

    [[nodiscard]] RiverFlowField BakeRiverFlow( std::span<const glm::vec3> points, float width, float flowSpeed,
                                                float flowPeriod, float cmPerTexel ) noexcept;

    /// One river body as the ECS authors it (ECS::WaterBodyData + the entity's location): what a bake reads.
    struct RiverBakeInput
    {
        std::span<const glm::vec3> LocalPoints; // relative to Origin
        glm::vec3                  Origin{ 0.0f };
        float                      Width      = 800.0f;
        float                      FlowSpeed  = 150.0f;
        float                      FlowPeriod = 1.0f;
    };

    /// The texel size every river is baked at, cm (BakeRiverFlow coarsens it for a river longer than
    /// kRiverFlowMaxSize texels).
    inline constexpr float kRiverFlowCmPerTexel = 50.0f;

    /// A hash of everything a bake reads: equal keys = the same field, so a caller re-bakes only on a change.
    [[nodiscard]] uint64_t RiverBakeKey( const RiverBakeInput& input ) noexcept;

    /// The world-space bake of @p input at kRiverFlowCmPerTexel; an empty field (Size 0) for a river that cannot
    /// be drawn (fewer than two points, a non-positive width).
    [[nodiscard]] std::shared_ptr<const RiverFlowField> BakeRiver( const RiverBakeInput& input ) noexcept;
} // namespace Desert::Water
