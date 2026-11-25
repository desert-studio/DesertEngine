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
    /// THE WATERFALL (UE 5.x Water: a river's steep reach is drawn as a falling sheet over the river body, Niagara
    /// foam / spray / mist at its foot). Here the fall is not a body of its own: it IS the river's spline where the
    /// spline drops steeper than kRiverFallMinSlope, so the river and its fall have one source (the spline) and one
    /// velocity (the bake). A river poured off a cliff makes its waterfall by itself.
    ///
    /// The rise over run (cm/cm) above which a stretch of the spline falls instead of flowing: 45 degrees.
    inline constexpr float kRiverFallMinSlope = 1.0f;
    /// A steep stretch dropping less than this (cm) stays a sloped river surface (a riffle, not a fall).
    inline constexpr float kRiverFallMinDropCm = 50.0f;
    /// The falling sheet's thickness (cm): a point this close to the sheet's mid-plane is in the falling water
    /// (RiverFallAt) - what a floating body caught by the fall is dragged down by.
    inline constexpr float kRiverFallSheetThicknessCm = 40.0f;

    /// One sample of a fall's centre line (the river's spline samples over the steep reach, one flat sample of
    /// overlap at each end so the sheet meets the river surface above and below it).
    struct RiverFallSample
    {
        glm::vec3 Position{ 0.0f };  // world, cm: the sheet's centre line
        glm::vec3 Direction{ 0.0f }; // unit 3D tangent of the fall (down the sheet)
        float     Distance = 0.0f;   // along the 3D centre line from the fall's first sample, cm
        /// How far below the lip the sheet is here, cm (>= 0): the water's speed along the sheet follows from it
        /// (RiverFallSpeed). Geometry only, so the bake is the same whatever reads it.
        float Drop = 0.0f;
    };

    /// THE VELOCITY FROM THE SLOPE: the water leaves the lip at the river's @p flowSpeed (cm/s) and gains by free
    /// fall down the sheet, v^2 = v0^2 + 2 g dh (@p drop cm below the lip, @p gravity cm/s^2: the scene's).
    [[nodiscard]] float RiverFallSpeed( float flowSpeed, float drop, float gravity ) noexcept;

    /// One waterfall of a river (a run of steep spline stretches), world space.
    struct RiverFall
    {
        std::vector<RiverFallSample> Samples; // >= 2
        /// Unit horizontal vector across the sheet (the river's width direction at the lip).
        glm::vec3 Across{ 1.0f, 0.0f, 0.0f };
        float     Width = 0.0f; // the river's width, cm
        /// The lip (top of the steep reach) and the foot (where the fall meets the water below), world cm.
        glm::vec3 Lip{ 0.0f };
        glm::vec3 Foot{ 0.0f };
        /// The river's FlowSpeed (cm/s): the speed the water leaves the lip at (RiverFallSpeed's v0).
        float FlowSpeed = 0.0f;
        /// The lip's height over the foot, cm.
        float Height = 0.0f;
        /// The river's flow distance (RiverFlowField texel w) at the lip, cm: the sheet's flow phase continues the
        /// river's, so the texture runs over the edge without a seam.
        float LipFlowDistance = 0.0f;
    };

    /// What a point meets in a fall's sheet (RiverFallAt).
    struct RiverFallHit
    {
        glm::vec3 SheetPoint{ 0.0f }; // the closest point of the sheet's mid-plane, world cm
        glm::vec3 Normal{ 0.0f };     // the sheet's normal on the point's side
        glm::vec3 Velocity{ 0.0f };   // the falling water's velocity there, cm/s (down the sheet)
        float     Immersion = 0.0f;   // kRiverFallSheetThicknessCm / 2 minus the distance to the mid-plane, > 0
    };

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
        /// The river's waterfalls (its steep reaches), upstream first. The texels whose nearest spline stretch is
        /// steep carry no bank mask: the fall's sheet is drawn there instead of the flat surface, and the CPU
        /// answers the sheet (RiverFallAt), not a level.
        std::vector<RiverFall> Falls;
    };

    /// The largest side a flow field is baked at (texels): a longer river gets coarser texels, not a bigger image.
    inline constexpr uint32_t kRiverFlowMaxSize = 1024;

    /// THE BANK CLIP (UE: the river's WaterInfo mask cuts its surface at the spline's width): the bilinear bank
    /// mask at or above this is water, below it is no water - for the drawn surface (WaterMeshTiles.glslh
    /// WaterOutsideBank, `WATER_RIVER_BANK_CLIP`) and for every CPU query (WaterBodyCovers) alike.
    inline constexpr float kRiverBankClip = 0.5f;

    /// The flow at @p xz (world XZ, cm) exactly as the water surface reads it (WaterRiver.glslh WaterFlowAt):
    /// xy = the surface velocity (cm/s), z = the bank mask, bilinear between the texel centres, a neighbour off
    /// the field counting as zero. Zero for an empty field.
    [[nodiscard]] glm::vec3 SampleRiverFlowBilinear( const RiverFlowField& field, glm::vec2 xz ) noexcept;

    /// Whether @p xz (world XZ, cm) is inside the banks: SampleRiverFlowBilinear's mask >= kRiverBankClip, the
    /// very test the surface discards by, so the CPU never answers water where no water is drawn.
    [[nodiscard]] bool RiverInsideBank( const RiverFlowField& field, glm::vec2 xz ) noexcept;

    /// The still surface's height at @p xz (world XZ, cm): SurfaceHeightCm filtered bilinearly over the texels
    /// inside the banks only (a bank texel's 0 never drags the level down); nullopt outside the banks or the field.
    [[nodiscard]] std::optional<float> SampleRiverSurfaceY( const RiverFlowField& field, glm::vec2 xz ) noexcept;

    /// The falling sheet at @p point (world cm): the nearest fall stretch whose mid-plane lies within half of
    /// kRiverFallSheetThicknessCm of the point and whose width spans it, its water falling at RiverFallSpeed under
    /// @p gravity (cm/s^2, the scene's); nullopt elsewhere.
    [[nodiscard]] std::optional<RiverFallHit> RiverFallAt( const RiverFlowField& field, const glm::vec3& point,
                                                           float gravity ) noexcept;

    /// Bakes @p points (already in world space) into a flow field of about @p cmPerTexel (clamped so neither side
    /// exceeds kRiverFlowMaxSize), its steep reaches found as falls (RiverFlowField::Falls). An empty field
    /// (Size 0) for fewer than two points or a non-positive width.
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
