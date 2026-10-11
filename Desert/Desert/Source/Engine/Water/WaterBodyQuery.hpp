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

#include <Engine/Water/GerstnerWaterWaves.hpp>
#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Water
{
    /// The depth an ocean reports where no landscape answers under the point, or the landscape stands above the
    /// plane (UE r.Water.OceanFallbackDepth, its default).
    inline constexpr float kOceanFallbackDepthCm = 3000.0f;

    /// One ocean as the physics step sees it (UE FSolverSafeWaterBodyData): plain data, safe to read off the
    /// game thread.
    struct WaterBodyState
    {
        /// The body's world location: Y is the still water level, (X, Z) is the WaveOrigin the waves are
        /// evaluated relative to and the centre of the footprint.
        glm::vec3 Location{ 0.0f };
        /// Full size of the footprint along world X and Z, cm (UE OceanExtents).
        glm::vec2 Extents{ 51200.0f, 51200.0f };
        /// Empty = a flat body (UE HasWaves() false). Shared: every body naming one `.dwaves` reads one set.
        std::shared_ptr<const std::vector<GerstnerWave>> Waves;
        /// Depth at which the waves have died to e^-2 (UE TargetWaveMaskDepth, its default).
        float TargetWaveMaskDepth = 2048.0f;
    };

    struct WaterQueryResult
    {
        glm::vec3 PlaneLocation{ 0.0f };
        glm::vec3 SurfaceLocation{ 0.0f };
        glm::vec3 PlaneNormal{ 0.0f, 1.0f, 0.0f };
        glm::vec3 SurfaceNormal{ 0.0f, 1.0f, 0.0f };
        /// Plane to the ground under it, >= 0.
        float PlaneDepth = 0.0f;
        /// PlaneDepth plus the attenuated wave height.
        float SurfaceDepth = 0.0f;
        /// The attenuated wave height above the plane.
        float WaveHeight = 0.0f;
        /// The share of the waves that survives here, [0, 1].
        float WaveAttenuation = 0.0f;
        /// The highest the attenuated waves can rise here (UE FWaveInfo::MaxHeight).
        float MaxWaveHeight = 0.0f;
        /// How far below the surface the point is; negative above it.
        float ImmersionDepth = 0.0f;

        [[nodiscard]] bool IsInWater() const
        {
            return ImmersionDepth > 0.0f;
        }
    };

    /// Whether the point lies over the body's footprint (world X, Z).
    [[nodiscard]] bool WaterBodyCovers( const WaterBodyState& body, const glm::vec3& point );

    /// The landscape height under (x, z): the first tile that answers (neighbours share their seam samples).
    [[nodiscard]] std::optional<float> GroundHeightAt( std::span<const World::Landscape::LandscapeRayTile> ground,
                                                       float x, float z );

    /// UE QueryWaterInfoClosestToWorldLocation with ComputeLocation | ComputeNormal | ComputeDepth |
    /// ComputeImmersionDepth | IncludeWaves, for one ocean at wave time `time`.
    [[nodiscard]] WaterQueryResult QueryWaterBody( const WaterBodyState& body, const glm::vec3& point, float time,
                                                   std::optional<float> groundHeight );

    struct WaterQueryHit
    {
        std::size_t      Body = 0;
        WaterQueryResult Result;
    };

    /// The body the point is deepest in, of those that cover it; nullopt when it is in none.
    [[nodiscard]] std::optional<WaterQueryHit>
    QueryWater( std::span<const WaterBodyState> bodies, std::span<const World::Landscape::LandscapeRayTile> ground,
                const glm::vec3& point, float time );

    /**
     * @brief The water of one world: its bodies, the ground under them and the wave clock (UE UWaterSubsystem).
     *
     * THE CLOCK IS THE PHYSICS STEP'S: Advance is called with each step's seconds, so the server and every
     * client that steps the same steps sees the same waves; SetTime adopts a time the server sent.
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

        /// Replaces the bodies and the ground; the ground's heights are BORROWED and must outlive the queries.
        void SetWorld( std::vector<WaterBodyState>                     bodies,
                       std::vector<World::Landscape::LandscapeRayTile> ground );

        [[nodiscard]] std::span<const WaterBodyState> Bodies() const
        {
            return m_Bodies;
        }

        [[nodiscard]] std::optional<WaterQueryHit> Query( const glm::vec3& point ) const;

    private:
        double                                          m_Time = 0.0;
        std::vector<WaterBodyState>                     m_Bodies;
        std::vector<World::Landscape::LandscapeRayTile> m_Ground;
    };
} // namespace Desert::Water
