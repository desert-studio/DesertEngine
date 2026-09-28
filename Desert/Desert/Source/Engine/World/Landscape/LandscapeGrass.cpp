#include <Engine/World/Landscape/LandscapeGrass.hpp>

#include <Common/Core/Math/Pcg32.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::World::Landscape
{
    namespace
    {
        using Assets::Serialization::GrassFloatInterval;
        using Assets::Serialization::GrassScaling;
        using Assets::Serialization::GrassVariety;

        // The placement stream the foliage brush draws from too: integer PCG32, the same numbers on every
        // compiler (UE uses FRandomStream's LCG here; the pattern — one stream per cell, drawn in a fixed order —
        // is kept, the generator is this project's one).
        using GrassRandomStream = Common::Math::Pcg32;

        float Interpolate( const GrassFloatInterval& range, float alpha )
        {
            return range.Min + alpha * ( range.Max - range.Min );
        }

        // UE FGrassBuilderBase::GetRandomScale, without the weight attenuation this port refuses.
        glm::vec3 RandomScale( const GrassVariety& v, GrassRandomStream& random )
        {
            glm::vec3 scale;
            switch ( v.Scaling )
            {
                case GrassScaling::Uniform:
                    scale = glm::vec3( Interpolate( v.ScaleX, random.Next01() ) );
                    break;
                case GrassScaling::Free:
                    // UE's axes, Z up: ScaleY is this engine's Z, ScaleZ (the height) this engine's Y.
                    scale.x = Interpolate( v.ScaleX, random.Next01() );
                    scale.z = Interpolate( v.ScaleY, random.Next01() );
                    scale.y = Interpolate( v.ScaleZ, random.Next01() );
                    break;
                case GrassScaling::LockXY:
                    scale.x = Interpolate( v.ScaleX, random.Next01() );
                    scale.z = scale.x;
                    // Y is up here: UE's Z (the height) is this engine's Y.
                    scale.y = Interpolate( v.ScaleZ, random.Next01() );
                    break;
            }
            return scale;
        }

        // The rotation taking +Y onto @p normal (UE ApplyNormalAlignmentTransform, Y-up).
        glm::mat4 AlignToNormal( const glm::vec3& normal )
        {
            const glm::vec3 up( 0.0f, 1.0f, 0.0f );
            const glm::vec3 n    = glm::normalize( normal );
            const glm::vec3 axis = glm::cross( up, n );
            const float     sinA = glm::length( axis );
            const float     cosA = glm::dot( up, n );
            if ( sinA < 1.0e-6f )
                return glm::mat4( 1.0f );
            return glm::rotate( glm::mat4( 1.0f ), std::atan2( sinA, cosA ), axis / sinA );
        }

        uint32_t Mix( uint32_t h )
        {
            // MurmurHash3's fmix32: every input bit reaches every output bit.
            h ^= h >> 16;
            h *= 0x85EBCA6Bu;
            h ^= h >> 13;
            h *= 0xC2B2AE35u;
            h ^= h >> 16;
            return h;
        }
    } // namespace

    GrassCellCoord GrassCellAt( float worldX, float worldZ )
    {
        return { static_cast<int32_t>( std::floor( worldX / kLandscapeGrassCellCm ) ),
                 static_cast<int32_t>( std::floor( worldZ / kLandscapeGrassCellCm ) ) };
    }

    uint32_t GrassCellSeed( GrassCellCoord cell, uint32_t salt )
    {
        uint32_t h = Mix( salt ^ 0x9E3779B9u );
        h          = Mix( h ^ static_cast<uint32_t>( cell.X ) );
        h          = Mix( h ^ ( static_cast<uint32_t>( cell.Z ) * 0x27D4EB2Fu ) );
        return h;
    }

    float GrassHalton( uint32_t index, uint32_t base )
    {
        float       result   = 0.0f;
        const float invBase  = 1.0f / static_cast<float>( base );
        float       fraction = invBase;
        while ( index > 0u )
        {
            result += static_cast<float>( index % base ) * fraction;
            index /= base;
            fraction *= invBase;
        }
        return result;
    }

    uint32_t GrassCandidatesPerSide( const GrassVariety& variety )
    {
        // UE LandscapeGrass.cpp:2070 — the density is per 1000 x 1000 cm.
        const double area = static_cast<double>( kLandscapeGrassCellCm ) * kLandscapeGrassCellCm;
        return static_cast<uint32_t>(
             std::ceil( std::sqrt( std::abs( area * variety.GrassDensity / 1000.0 / 1000.0 ) ) ) );
    }

    Graphic::InstanceCullDistance GrassCullDistance( const GrassVariety& variety )
    {
        return { variety.StartCullDistance, variety.EndCullDistance };
    }

    std::vector<glm::mat4> GenerateGrassCell( const GrassVariety& variety, GrassCellCoord cell, uint32_t salt,
                                              const GrassSurfaceSampler& surface )
    {
        const uint32_t seed = GrassCellSeed( cell, salt );
        // UE keys a component's Halton sequence by a non-zero base index; here the cell's seed picks it.
        const uint32_t    haltonBase = 1u + ( seed & 0xFFFFu );
        GrassRandomStream random( seed );

        const uint32_t side  = GrassCandidatesPerSide( variety );
        const uint32_t count = side * side;
        const float    x0    = static_cast<float>( cell.X ) * kLandscapeGrassCellCm;
        const float    z0    = static_cast<float>( cell.Z ) * kLandscapeGrassCellCm;

        std::vector<glm::mat4> out;
        for ( uint32_t i = 0; i < count; ++i )
        {
            const float x      = x0 + GrassHalton( i + haltonBase, 2u ) * kLandscapeGrassCellCm;
            const float z      = z0 + GrassHalton( i + haltonBase, 3u ) * kLandscapeGrassCellCm;
            const auto  sample = surface( x, z );
            if ( !sample )
                continue;
            const float w = sample->Weight;
            // UE's short circuit is kept: the random fraction is drawn only for a weight inside the range, so a
            // stroke outside one sample does not reshuffle every later instance of the cell.
            const bool keep = w > variety.AllowedDensityRange.Min && w <= variety.AllowedDensityRange.Max &&
                              w >= random.Next01();
            if ( !keep )
                continue;
            const glm::vec3 scale = RandomScale( variety, random );
            const float     yaw   = variety.RandomRotation ? random.Next01() * 360.0f : 0.0f;

            glm::mat4 m = glm::translate( glm::mat4( 1.0f ), glm::vec3( x, sample->HeightCm, z ) );
            if ( variety.AlignToSurface )
                m = m * AlignToNormal( sample->Normal );
            m = glm::rotate( m, glm::radians( yaw ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
            m = glm::scale( m, scale );
            out.push_back( m );
        }
        return out;
    }

    std::optional<float> SampleLandscapeWeight( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                size_t layer, float worldX, float worldZ )
    {
        if ( layer >= tile.WeightLayers().size() )
            return std::nullopt;
        const float u     = ( worldX - frame.OriginX ) / frame.SpacingCm;
        const float v     = ( worldZ - frame.OriginZ ) / frame.SpacingCm;
        const auto  lastX = static_cast<float>( tile.SamplesX() - 1u );
        const auto  lastZ = static_cast<float>( tile.SamplesZ() - 1u );
        // A NaN coordinate must land outside: the negated form rejects it, De Morgan's rewrite would not.
        // NOLINTNEXTLINE(readability-simplify-boolean-expr)
        if ( !( u >= 0.0f && v >= 0.0f && u <= lastX && v <= lastZ ) )
            return std::nullopt;
        const uint32_t x0 = std::min( static_cast<uint32_t>( u ), tile.SamplesX() - 2u );
        const uint32_t z0 = std::min( static_cast<uint32_t>( v ), tile.SamplesZ() - 2u );
        const float    fx = u - static_cast<float>( x0 );
        const float    fz = v - static_cast<float>( z0 );
        const auto     w  = [&]( uint32_t x, uint32_t z )
        { return static_cast<float>( tile.Weight( layer, x, z ) ) / 255.0f; };
        const float a = w( x0, z0 ) + ( w( x0 + 1u, z0 ) - w( x0, z0 ) ) * fx;
        const float b = w( x0, z0 + 1u ) + ( w( x0 + 1u, z0 + 1u ) - w( x0, z0 + 1u ) ) * fx;
        return a + ( b - a ) * fz;
    }

    std::vector<GrassCellCoord> GrassCellsInRange( glm::vec2 cameraXZ, float radiusCm )
    {
        const GrassCellCoord lo = GrassCellAt( cameraXZ.x - radiusCm, cameraXZ.y - radiusCm );
        const GrassCellCoord hi = GrassCellAt( cameraXZ.x + radiusCm, cameraXZ.y + radiusCm );

        struct Ranked
        {
            float          Distance2;
            GrassCellCoord Cell;
        };
        std::vector<Ranked> ranked;
        for ( int32_t z = lo.Z; z <= hi.Z; ++z )
            for ( int32_t x = lo.X; x <= hi.X; ++x )
            {
                const glm::vec2 cellMin( static_cast<float>( x ) * kLandscapeGrassCellCm,
                                         static_cast<float>( z ) * kLandscapeGrassCellCm );
                const glm::vec2 nearest = glm::clamp( cameraXZ, cellMin, cellMin + kLandscapeGrassCellCm );
                const glm::vec2 d       = nearest - cameraXZ;
                if ( glm::dot( d, d ) > radiusCm * radiusCm )
                    continue;
                const glm::vec2 c = cellMin + 0.5f * kLandscapeGrassCellCm - cameraXZ;
                ranked.push_back( { glm::dot( c, c ), { x, z } } );
            }
        std::sort( ranked.begin(), ranked.end(), []( const Ranked& a, const Ranked& b )
                   { return a.Distance2 != b.Distance2 ? a.Distance2 < b.Distance2 : a.Cell < b.Cell; } );
        std::vector<GrassCellCoord> out;
        out.reserve( ranked.size() );
        for ( const Ranked& r : ranked )
            out.push_back( r.Cell );
        return out;
    }

    GrassCellStreamer::TickResult GrassCellStreamer::Tick( glm::vec2 cameraXZ, float radiusCm, uint32_t cellBudget,
                                                           const Generator& generate )
    {
        TickResult                        result;
        const std::vector<GrassCellCoord> wanted = GrassCellsInRange( cameraXZ, radiusCm );

        for ( auto it = m_Cells.begin(); it != m_Cells.end(); )
        {
            if ( std::find( wanted.begin(), wanted.end(), it->first ) == wanted.end() )
            {
                it = m_Cells.erase( it );
                ++result.Evicted;
                m_Changed = true;
            }
            else
                ++it;
        }
        for ( const GrassCellCoord& cell : wanted )
        {
            if ( m_Cells.contains( cell ) )
                continue;
            if ( result.Generated >= cellBudget )
            {
                ++result.Missing;
                continue;
            }
            m_Cells.emplace( cell, generate( cell ) );
            ++result.Generated;
            m_Changed = true;
        }
        return result;
    }

    void GrassCellStreamer::Invalidate( glm::vec2 worldMin, glm::vec2 worldMax )
    {
        const GrassCellCoord lo = GrassCellAt( worldMin.x, worldMin.y );
        const GrassCellCoord hi = GrassCellAt( worldMax.x, worldMax.y );
        for ( auto it = m_Cells.begin(); it != m_Cells.end(); )
        {
            const GrassCellCoord c = it->first;
            if ( c.X >= lo.X && c.X <= hi.X && c.Z >= lo.Z && c.Z <= hi.Z )
            {
                it        = m_Cells.erase( it );
                m_Changed = true;
            }
            else
                ++it;
        }
    }

    std::shared_ptr<const std::vector<glm::mat4>> GrassCellStreamer::Instances()
    {
        if ( m_Changed || !m_Instances )
        {
            auto all = std::make_shared<std::vector<glm::mat4>>();
            for ( const auto& [cell, instances] : m_Cells )
                all->insert( all->end(), instances.begin(), instances.end() );
            m_Instances = std::move( all );
            m_Changed   = false;
        }
        return m_Instances;
    }
} // namespace Desert::World::Landscape
