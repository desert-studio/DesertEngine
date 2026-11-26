#pragma once

// The water query (WATER-W2): what a point in the world meets in the water — the still plane, the surface the
// waves raise over it, the depth of the water under it and how far the point is immersed.
//
// Ported from UE 5.8 Engine/Plugins/Experimental/Water/Source/Runtime: Private/WaterBodyComponent.cpp:645-866
// (UWaterBodyComponent::QueryWaterInfoClosestToWorldLocation), :2012-2051 (GetWaveInfoAtPosition),
// Public/WaterBodyTypes.h:99 (IsInWater), Public/BuoyancyComponentSimulation.h:191-231 (of several bodies the
// one the point is DEEPEST in answers), Private/WaterSubsystem.cpp:242-251, :510-525 (the wave clock).
// Adapted: Y-up — UE's Z is our Y and UE's (X, Y) plane is our (X, Z); the body is a plain state struct read by
// the physics step (UE's FSolverSafeWaterBodyData), not a UObject; UE's LWC tile is the body's WaveOrigin, its
// own location. Ocean only: lakes and rivers take their shape and depth from a spline (W-9, W-10).

#include <Water/Public/FFTSurfaceMirror.hpp>
#include <Water/Public/WaterRiver.hpp>
#include <Water/Public/WaterShoreField.hpp>
#include <Water/Public/WaterWaveTable.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Water
{
    struct GerstnerWave; // Water/Public/GerstnerWaterWaves.hpp: held by shared pointer, evaluated in the .cpp

    /// One ocean as the physics step sees it (UE FSolverSafeWaterBodyData): plain data, safe to read off the
    /// game thread.
    struct WaterBodyState
    {
        /// The body's identity in its world (its entity): what a kept answer is kept for.
        std::uint32_t Id = 0;
        /// The body's world location: Y is the still water level, (X, Z) the centre of the footprint and, unless
        /// WaveOrigin says another, the origin the waves are evaluated relative to.
        glm::vec3 Location{ 0.0f };
        /// Full size of the footprint along world X and Z, cm (UE OceanExtents).
        glm::vec2 Extents{ 51200.0f, 51200.0f };
        /// Empty = a flat body (UE HasWaves() false). Shared: every body naming one `.dwaves` reads one set.
        std::shared_ptr<const std::vector<GerstnerWave>> Waves;
        /// The lane's WaveOrigin (WaterWaveLane::Origin): a river's footprint centre is not its entity's X, Z.
        std::optional<glm::vec2> WaveOrigin;
        /// The waves are not known yet (WaterWaveState::Pending): the answer is the still plane, not ready.
        bool WavesPending = false;
        /// Ocean or River (UE AWaterBodyOcean / AWaterBodyRiver). A river's Location is its footprint's centre at
        /// its mean level, Extents its footprint, and it covers only where its baked bank mask is set.
        WaterBodyType Type = WaterBodyType::Ocean;
        /// A river's baked flow (null for an ocean): the bank mask the queries clip to and the current they read.
        std::shared_ptr<const RiverFlowField> Flow;
        /// The drawn surface is the FFT ocean (WaterWaveState::FFT, the renderer's own choice): the queries
        /// answer with FFTSurface, the cascades the GPU drew, never with the set's Gerstner generator.
        bool FFT = false;
        /// The newest read of that ocean; null until one landed, and the answer is then the still plane, not ready.
        std::shared_ptr<const FFTSurfaceSnapshot> FFTSurface;
        /// The fluid's density, kg/m^3, and drag, 1/s (ECS::WaterBodyData::Density / Drag): what a buoyant body's
        /// pontoons in this body are pushed up and held back by (BuoyancySimulation.hpp).
        float Density = 1000.0f;
        float Drag    = 1.5f;
        /// The plunges that land in this body (WaterBodyGather: the highest-priority body covering each foot): their
        /// boil adds to the body's current (PlungeVelocityAt).
        std::vector<FallPlunge> Plunges;
    };

    /// Gives the body its lane's waves: what the renderer draws it with.
    void SetWaveLane( WaterBodyState& body, const WaterWaveLane& lane, const WaterFFTSurface& surface );

    struct WaterQueryResult
    {
        glm::vec3 PlaneLocation{ 0.0f };
        glm::vec3 SurfaceLocation{ 0.0f };
        glm::vec3 PlaneNormal{ 0.0f, 1.0f, 0.0f };
        glm::vec3 SurfaceNormal{ 0.0f, 1.0f, 0.0f };
        /// Plane to the ground under it; negative where the ground stands above the plane (the swash's beach).
        float PlaneDepth = 0.0f;
        /// PlaneDepth plus the wave height: what stands between the surface and the ground.
        float SurfaceDepth = 0.0f;
        /// The waves' height above the plane, the depth's transform and the shore wave included.
        float WaveHeight = 0.0f;
        /// The factor the depth gives the longest band's amplitude (TransformBand); 0 on land.
        float WaveAttenuation = 0.0f;
        /// The highest the waves can rise here (UE FWaveInfo::MaxHeight).
        float MaxWaveHeight = 0.0f;
        /// The waves are known. False: the body's set is still being read, or it draws an FFT ocean no read-back of
        /// which has landed yet, and the surface is the still plane (what floats keeps its last answer).
        bool SurfaceReady = true;
        /// How far below the surface the point is; negative above it.
        float ImmersionDepth = 0.0f;
        /// The water's current at the point, cm/s (UE FWaterQueryResult::Velocity): a river's baked flow on the
        /// (X, Z) plane, plus the outward push of every fall's plunge landing in the body (WaterBodyState::Plunges).
        glm::vec3 Velocity{ 0.0f };

        [[nodiscard]] bool IsInWater() const
        {
            return ImmersionDepth > 0.0f;
        }
    };

    /// Whether the point lies over the body's footprint (world X, Z).
    [[nodiscard]] bool WaterBodyCovers( const WaterBodyState& body, const glm::vec3& point );

    /// UE QueryWaterInfoClosestToWorldLocation with ComputeLocation | ComputeNormal | ComputeDepth |
    /// ComputeImmersionDepth | IncludeWaves, for one body at wave time `time`, over the
    /// ground and the shore of @p shore (null: a world whose field is not baked yet answers the open sea).
    [[nodiscard]] WaterQueryResult QueryWaterBody( const WaterBodyState& body, const glm::vec3& point, float time,
                                                   const WaterShoreField* shore );

    struct WaterQueryHit
    {
        std::size_t      Body = 0;
        WaterQueryResult Result;
    };

    /// The body the point is deepest in, of those of the highest WaterBodyPriority that cover it (the surface the
    /// renderer draws there: inside a river's banks the river, never the ocean under it); nullopt when it is in
    /// none of them. With @p andUnready a body over the point whose surface is not ready (WaterQueryResult::
    /// SurfaceReady) answers too, whether or not the point is under its still plane.
    [[nodiscard]] std::optional<WaterQueryHit>
    QueryWater( std::span<const WaterBodyState> bodies, const WaterShoreField* shore, const glm::vec3& point,
                float time, bool andUnready = false );

    /**
     * @brief The water of one world: its bodies, the ground under them and the wave clock (UE UWaterSubsystem).
     *
     * ONE WATER CLOCK (UE UWaterSubsystem::GetWaterTimeSeconds): the scene's game time, the clock the renderer
     * draws the Gerstner waves and the FFT cascades at and stamps the FFT readback with. Physics SetTime's the
     * game time at its frame's start and Advance's it by each fixed step, so a query in a step reads the surface
     * drawn around it (and a networked game time is the waves' sync, with nothing water-specific to send).
     */
    class WaterSubsystem
    {
    public:
        void Advance( double stepSeconds );
        void SetTime( double seconds );
        [[nodiscard]] float Time() const
        {
            return static_cast<float>( m_Time );
        }

        /// Replaces the bodies and the shore field (immutable, the one the renderer draws over). The body rows
        /// are copied into the subsystem's own storage, reused frame to frame (the bodies' wave sets are shared,
        /// not copied), so a steady frame allocates nothing.
        void SetWorld( std::span<const WaterBodyState> bodies, std::shared_ptr<const WaterShoreField> shore );

        [[nodiscard]] std::span<const WaterBodyState> Bodies() const
        {
            return m_Bodies;
        }

        [[nodiscard]] std::optional<WaterQueryHit> Query( const glm::vec3& point, bool andUnready = false ) const;

    private:
        double                                 m_Time = 0.0;
        std::vector<WaterBodyState>            m_Bodies;
        std::shared_ptr<const WaterShoreField> m_Shore;
    };
} // namespace Desert::Water
