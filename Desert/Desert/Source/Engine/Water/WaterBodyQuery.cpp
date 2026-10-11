#include <Engine/Water/WaterBodyQuery.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <utility>

namespace Desert::Water
{
    bool WaterBodyCovers( const WaterBodyState& body, const glm::vec3& point )
    {
        const glm::vec2 half = body.Extents * 0.5f;
        return std::abs( point.x - body.Location.x ) <= half.x && std::abs( point.z - body.Location.z ) <= half.y;
    }

    std::optional<float> GroundHeightAt( std::span<const World::Landscape::LandscapeRayTile> ground, float x,
                                         float z )
    {
        for ( const World::Landscape::LandscapeRayTile& tile : ground )
        {
            if ( tile.Heights == nullptr )
                continue;
            if ( const std::optional<float> height =
                      World::Landscape::SampleLandscapeHeight( *tile.Heights, tile.Frame, x, z ) )
                return height;
        }
        return std::nullopt;
    }

    WaterQueryResult QueryWaterBody( const WaterBodyState& body, const glm::vec3& point, float time,
                                     std::optional<float> groundHeight )
    {
        WaterQueryResult result;

        // The plane: an ocean's surface is level, at the body's own height (UE :677-701).
        result.PlaneLocation   = glm::vec3( point.x, body.Location.y, point.z );
        result.SurfaceLocation = result.PlaneLocation;

        // The depth: down to the landscape; an ocean over no landscape, or under it, reports the fallback depth
        // and has its waves cancelled where the landscape stands above the plane (UE :743-794).
        float waveAttenuation = 1.0f;
        float planeDepth      = 0.0f;
        if ( groundHeight )
        {
            planeDepth = result.PlaneLocation.y - *groundHeight;
            if ( planeDepth < 0.0f )
                waveAttenuation = 0.0f;
        }
        if ( !groundHeight || planeDepth < 0.0f )
            planeDepth = kOceanFallbackDepthCm;
        result.PlaneDepth   = glm::max( planeDepth, 0.0f );
        result.SurfaceDepth = result.PlaneDepth;

        // The waves: attenuated by depth, evaluated at the fixed plane point relative to the WaveOrigin
        // (UE :801-831 and GetWaveInfoAtPosition :2012-2051).
        if ( body.Waves && !body.Waves->empty() )
        {
            const std::span<const GerstnerWave> waves( *body.Waves );
            waveAttenuation *= WaveDepthAttenuation( result.SurfaceDepth, body.TargetWaveMaskDepth );
            result.WaveAttenuation = waveAttenuation;
            if ( waveAttenuation > 0.0f )
            {
                result.MaxWaveHeight = MaxWaveHeight( waves ) * waveAttenuation;

                const glm::vec2        plane( point.x - body.Location.x, point.z - body.Location.z );
                const WaveHeightSample sample = WaveHeightAt( waves, plane, time );

                // UE lerps the normal toward the plane's by the same share and leaves it unnormalised; a
                // surface normal is a direction, so it is normalised here.
                const glm::vec3 normal = glm::mix( result.PlaneNormal, sample.Normal, waveAttenuation );
                if ( glm::dot( normal, normal ) > 0.0f )
                    result.SurfaceNormal = glm::normalize( normal );

                result.WaveHeight = sample.Height * waveAttenuation;
                result.SurfaceLocation.y += result.WaveHeight;
                result.SurfaceDepth += result.WaveHeight;
            }
        }

        // Immersion: how far under the waved surface the point is (UE :834-842).
        result.ImmersionDepth = result.SurfaceLocation.y - point.y;
        return result;
    }

    std::optional<WaterQueryHit> QueryWater( std::span<const WaterBodyState>                     bodies,
                                             std::span<const World::Landscape::LandscapeRayTile> ground,
                                             const glm::vec3& point, float time )
    {
        // Of several bodies, the one the point is deepest in (UE BuoyancyComponentSimulation.h:199-231).
        std::optional<WaterQueryHit> best;
        std::optional<float>         groundHeight;
        bool                         groundSampled = false;
        for ( std::size_t i = 0; i < bodies.size(); ++i )
        {
            if ( !WaterBodyCovers( bodies[i], point ) )
                continue;
            if ( !groundSampled )
            {
                groundHeight  = GroundHeightAt( ground, point.x, point.z );
                groundSampled = true;
            }
            const WaterQueryResult result = QueryWaterBody( bodies[i], point, time, groundHeight );
            if ( result.IsInWater() && ( !best || result.ImmersionDepth > best->Result.ImmersionDepth ) )
                best = WaterQueryHit{ i, result };
        }
        return best;
    }

    void WaterSubsystem::Advance( double stepSeconds )
    {
        m_Time += stepSeconds;
    }

    void WaterSubsystem::SetTime( double seconds )
    {
        m_Time = seconds;
    }

    void WaterSubsystem::SetWorld( std::vector<WaterBodyState>                     bodies,
                                   std::vector<World::Landscape::LandscapeRayTile> ground )
    {
        m_Bodies = std::move( bodies );
        m_Ground = std::move( ground );
    }

    std::optional<WaterQueryHit> WaterSubsystem::Query( const glm::vec3& point ) const
    {
        return QueryWater( m_Bodies, m_Ground, point, Time() );
    }
} // namespace Desert::Water
