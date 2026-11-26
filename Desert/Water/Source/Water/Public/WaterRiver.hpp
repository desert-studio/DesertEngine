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
    /// A sheet's drop below the receiving water's level, cm: the falling water plunges on under the surface, so a
    /// wave trough never opens a gap under it (the receiving surface hides it by depth / WaterFallUnderRiver).
    inline constexpr float kRiverFallPlungeCm = 100.0f;
    /// The samples of one fall's flight from its lip to its plunge (the sheet's rows), plus one flat sample of
    /// overlap on the river above the lip.
    inline constexpr uint32_t kRiverFallSheetSamples = 16u;
    /// THE PLUNGE (the white boil where the fall lands): its radius past the sheet's half width per cm of drop, and
    /// the share of the landing speed the surface current leaves the landing line with, decaying to 0 at the radius
    /// (calibrated on the reference frame: Seljalandsfoss, 60 m drop, boil ~25-30 m across).
    inline constexpr float kPlungeRadiusPerDrop = 0.5f;
    inline constexpr float kPlungeOutflowShare  = 0.3f;
    /// A river sample this close above an ocean's level (cm) inside its footprint is the river's mouth (it covers
    /// the spline's Catmull-Rom overshoot, ~11 cm on Water_Falls).
    inline constexpr float kRiverMouthToleranceCm = 20.0f;

    /// One sample of a fall's centre line: the water's flight (RiverFall), uniform in flight time, after one flat
    /// sample of overlap on the river above the lip so the sheet starts on the river's surface.
    struct RiverFallSample
    {
        glm::vec3 Position{ 0.0f };  // world, cm: the sheet's centre line (the water's flight, see RiverFall)
        glm::vec3 Direction{ 0.0f }; // unit 3D tangent (the water's velocity direction)
        float     Distance = 0.0f;   // along the centre line from the first sample, cm
        float     Drop     = 0.0f;   // below the lip, cm (>= 0)
        float     Speed    = 0.0f;   // the water's speed here, cm/s: sqrt(v0^2 + 2 g Drop), baked once
    };

    /// Where a fall lands (the boil): the foam source and the surface current away from the landing line.
    struct FallPlunge
    {
        glm::vec3 Foot{ 0.0f };             // the landing line's centre on the receiving level, world cm
        glm::vec3 Across{ 1.0f, 0.0f, 0.0f };
        glm::vec2 Downstream{ 1.0f, 0.0f }; // unit XZ: the flight's horizontal direction
        float     HalfWidth    = 0.0f;      // cm
        float     Radius       = 0.0f;      // HalfWidth + kPlungeRadiusPerDrop * Height, cm
        float     LandingSpeed = 0.0f;      // cm/s
        float     OutflowSpeed = 0.0f;      // kPlungeOutflowShare * LandingSpeed, cm/s at the landing line
    };

    /// One waterfall, world space. THE FLIGHT: the water leaves the lip horizontally at FlowSpeed and falls under
    /// the scene's gravity; where the authored bed (the spline) stands above that flight the water slides on the
    /// bed and leaves it again where the bed drops away (a chute and a free fall are one rule). The flight ends
    /// kRiverFallPlungeCm under the level below the steep run.
    struct RiverFall
    {
        std::vector<RiverFallSample> Samples; // kRiverFallSheetSamples + 1
        /// Unit horizontal vector across the sheet (the river's width direction at the lip).
        glm::vec3 Across{ 1.0f, 0.0f, 0.0f };
        float     Width = 0.0f; // the river's width, cm
        /// The lip (top of the steep reach), world cm.
        glm::vec3 Lip{ 0.0f };
        /// Where the flight meets the level below (Plunge.Foot), world cm.
        glm::vec3 Foot{ 0.0f };
        /// The river's FlowSpeed (cm/s): the speed the water leaves the lip at.
        float FlowSpeed = 0.0f;
        /// The lip's height over the level below, cm.
        float Height = 0.0f;
        /// The river's flow distance (RiverFlowField texel w) at the first sample, cm: the sheet's flow phase
        /// continues the river's, so the texture runs over the edge without a seam.
        float      LipFlowDistance = 0.0f;
        FallPlunge Plunge;
    };

    /// An ocean body a river may end in (WaterRiverBakes collects them; part of the river's bake key).
    struct RiverReceiver
    {
        uint32_t  Id = 0u;     // entt::entity of the ocean body
        glm::vec2 Min{ 0.0f }; // its footprint, world XZ cm
        glm::vec2 Max{ 0.0f };
        float     Level = 0.0f; // its still level, cm
    };

    /// Where a river enters an ocean (UE: the river-to-ocean transition, but for queries as well as for drawing):
    /// the first spline sample downstream of the river's last fall that lies inside a receiver's footprint at or
    /// below its level + kRiverMouthToleranceCm. The river's bake stops there; the receiver owns the water beyond.
    struct RiverMouth
    {
        uint32_t  Into = 0u;     // RiverReceiver::Id
        glm::vec2 Point{ 0.0f }; // world XZ, cm
        /// Over this distance upstream (cm) the river takes on the receiver's waves, 0 at the distance, 1 at the
        /// mouth: min(river width, the XZ distance from the last fall's plunge to the mouth); 0 when the last fall
        /// lands at or past the mouth (the fall pours straight into the receiver).
        float Radius = 0.0f;
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
        /// The ocean this river ends in; its texels stop at Mouth.Point (the receiver owns the water beyond).
        std::optional<RiverMouth> Mouth;
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
    /// kRiverFallSheetThicknessCm of the point and whose width spans it, its water moving at the baked
    /// RiverFallSample::Speed; nullopt elsewhere.
    [[nodiscard]] std::optional<RiverFallHit> RiverFallAt( const RiverFlowField& field,
                                                           const glm::vec3&      point ) noexcept;

    /// The surface current a plunge drives at @p xz (world XZ cm): away from the landing line,
    /// OutflowSpeed * (1 - d / Radius)^2 with d the distance to Foot; 0 past Radius. The GPU twin is
    /// WaterRiver.glslh WaterPlungeAt.
    [[nodiscard]] glm::vec2 PlungeVelocityAt( const FallPlunge& plunge, glm::vec2 xz ) noexcept;

    /// The share (0..1) of the receiver's waves a river carries at @p xz: 1 - |xz - Mouth.Point| / Mouth.Radius,
    /// clamped; 0 with no mouth or a zero radius. The GPU twin is WaterRiver.glslh WaterMouthWeight.
    [[nodiscard]] float RiverMouthWeight( const RiverFlowField& field, glm::vec2 xz ) noexcept;

    /// Bakes @p points (already in world space) into a flow field of about @p cmPerTexel (clamped so neither side
    /// exceeds kRiverFlowMaxSize), its steep reaches found as falls flown under @p gravity (cm/s^2, > 0) and its
    /// mouth into one of @p receivers (RiverFlowField::Mouth; the field stops there). An empty field (Size 0) for
    /// fewer than two points or a non-positive width.
    [[nodiscard]] RiverFlowField BakeRiverFlow( std::span<const glm::vec3> points, float width, float flowSpeed,
                                                float flowPeriod, float cmPerTexel, float gravity,
                                                std::span<const RiverReceiver> receivers ) noexcept;

    /// One river body as the ECS authors it (ECS::WaterBodyData + the entity's location) and the world it flows
    /// in: what a bake reads.
    struct RiverBakeInput
    {
        std::span<const glm::vec3>     LocalPoints; // relative to Origin
        glm::vec3                      Origin{ 0.0f };
        float                          Width      = 800.0f;
        float                          FlowSpeed  = 150.0f;
        float                          FlowPeriod = 1.0f;
        float                          Gravity    = 0.0f; // SceneSettings::Gravity, cm/s^2 (> 0)
        std::span<const RiverReceiver> Receivers;         // the world's unrefused ocean bodies
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
