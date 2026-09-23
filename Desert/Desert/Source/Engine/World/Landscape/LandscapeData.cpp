#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Engine/Assets/ContainerBytes.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Desert::World::Landscape
{
    uint16_t LandscapeSampleFromLocal( float localHeight )
    {
        const float scaled = localHeight * kLandscapeStepsPerLocal + static_cast<float>( kLandscapeMidSample );
        const float lo     = 0.0f;
        const float hi     = static_cast<float>( kLandscapeMaxSample );
        // FMath::Clamp's own spelling — see the header for why it is not std::clamp (NaN).
        const float clamped = scaled < lo ? lo : ( scaled < hi ? scaled : hi );
        // FMath::RoundToInt is floor(x + 0.5): an exact half step (a height on an odd multiple of 1/256
        // local unit) rounds UP, never to even.
        return static_cast<uint16_t>( std::floor( clamped + 0.5f ) );
    }

    uint16_t LandscapeSampleFromHeightCm( float heightCm, float zScale )
    {
        return LandscapeSampleFromLocal( heightCm / zScale );
    }

    Common::BoolResultStr ValidateLandscapeFrame( const LandscapeFrame& frame )
    {
        if ( !std::isfinite( frame.SpacingCm ) || frame.SpacingCm <= 0.0f )
            return Common::MakeFormattedError<bool>( "Landscape spacing must be a positive number of cm, got {}",
                                                     frame.SpacingCm );
        if ( !std::isfinite( frame.ZScale ) || frame.ZScale <= 0.0f )
            return Common::MakeFormattedError<bool>( "Landscape Z scale must be positive, got {}", frame.ZScale );
        if ( !std::isfinite( frame.OriginX ) || !std::isfinite( frame.OriginZ ) || !std::isfinite( frame.BaseY ) )
            return Common::MakeFormattedError<bool>( "Landscape origin must be finite, got ({}, {}, {})",
                                                     frame.OriginX, frame.BaseY, frame.OriginZ );
        return Common::MakeSuccess( true );
    }

    // ── The tile ──────────────────────────────────────────────────────────────────────────────────────

    namespace
    {
        Common::BoolResultStr ValidateDimensions( uint32_t samplesX, uint32_t samplesZ )
        {
            const auto inRange = []( uint32_t n )
            { return n >= kLandscapeMinTileSamples && n <= kLandscapeMaxTileSamples; };
            if ( !inRange( samplesX ) || !inRange( samplesZ ) )
                return Common::MakeFormattedError<bool>( "Landscape tile of {} x {} samples: each side must lie in "
                                                         "[{}, {}]",
                                                         samplesX, samplesZ, kLandscapeMinTileSamples,
                                                         kLandscapeMaxTileSamples );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr ValidateRect( const LandscapeRect& rect, uint32_t samplesX, uint32_t samplesZ )
        {
            if ( rect.Empty() )
                return Common::MakeFormattedError<bool>( "Landscape region [{}, {}) x [{}, {}) is empty", rect.X0,
                                                         rect.X1, rect.Z0, rect.Z1 );
            if ( rect.X1 > samplesX || rect.Z1 > samplesZ )
                return Common::MakeFormattedError<bool>( "Landscape region [{}, {}) x [{}, {}) leaves a tile of {} "
                                                         "x {} samples",
                                                         rect.X0, rect.X1, rect.Z0, rect.Z1, samplesX, samplesZ );
            return Common::MakeSuccess( true );
        }

        // True when the two rectangles overlap or share an edge or a corner. Merging those costs at most the
        // small notch beside them in the bounding box; merging far-apart ones would upload the whole gap.
        bool Touches( const LandscapeRect& a, const LandscapeRect& b )
        {
            return a.X0 <= b.X1 && b.X0 <= a.X1 && a.Z0 <= b.Z1 && b.Z0 <= a.Z1;
        }

        LandscapeRect Union( const LandscapeRect& a, const LandscapeRect& b )
        {
            return { std::min( a.X0, b.X0 ), std::min( a.Z0, b.Z0 ), std::max( a.X1, b.X1 ),
                     std::max( a.Z1, b.Z1 ) };
        }
    } // namespace

    LandscapeTileData::LandscapeTileData( uint32_t samplesX, uint32_t samplesZ, std::vector<uint16_t> samples )
        : m_SamplesX( samplesX ), m_SamplesZ( samplesZ ), m_Samples( std::move( samples ) )
    {
        MarkDirty( Bounds() );
    }

    Common::ResultStr<LandscapeTileData> LandscapeTileData::Create( uint32_t samplesX, uint32_t samplesZ )
    {
        if ( auto valid = ValidateDimensions( samplesX, samplesZ ); !valid )
            return Common::MakeError<LandscapeTileData>( valid.GetError() );
        std::vector<uint16_t> samples( static_cast<size_t>( samplesX ) * samplesZ, kLandscapeMidSample );
        return Common::MakeSuccess( LandscapeTileData( samplesX, samplesZ, std::move( samples ) ) );
    }

    Common::ResultStr<LandscapeTileData> LandscapeTileData::FromSamples( uint32_t samplesX, uint32_t samplesZ,
                                                                         std::vector<uint16_t> samples )
    {
        if ( auto valid = ValidateDimensions( samplesX, samplesZ ); !valid )
            return Common::MakeError<LandscapeTileData>( valid.GetError() );
        const size_t expected = static_cast<size_t>( samplesX ) * samplesZ;
        if ( samples.size() != expected )
            return Common::MakeFormattedError<LandscapeTileData>( "Landscape tile of {} x {} needs {} samples, got {}",
                                                                  samplesX, samplesZ, expected, samples.size() );
        return Common::MakeSuccess( LandscapeTileData( samplesX, samplesZ, std::move( samples ) ) );
    }

    uint16_t LandscapeTileData::Sample( uint32_t x, uint32_t z ) const
    {
        DESERT_VERIFY( x < m_SamplesX && z < m_SamplesZ, "Landscape sample ({}, {}) outside {} x {}", x, z,
                       m_SamplesX, m_SamplesZ );
        return m_Samples[static_cast<size_t>( z ) * m_SamplesX + x];
    }

    void LandscapeTileData::SetSample( uint32_t x, uint32_t z, uint16_t value )
    {
        DESERT_VERIFY( x < m_SamplesX && z < m_SamplesZ, "Landscape sample ({}, {}) outside {} x {}", x, z,
                       m_SamplesX, m_SamplesZ );
        uint16_t& slot = m_Samples[static_cast<size_t>( z ) * m_SamplesX + x];
        if ( slot == value )
            return;
        slot = value;
        MarkDirty( { x, z, x + 1u, z + 1u } );
    }

    Common::ResultStr<std::vector<uint16_t>> LandscapeTileData::ReadRegion( const LandscapeRect& rect ) const
    {
        if ( auto valid = ValidateRect( rect, m_SamplesX, m_SamplesZ ); !valid )
            return Common::MakeError<std::vector<uint16_t>>( valid.GetError() );
        std::vector<uint16_t> out;
        out.reserve( static_cast<size_t>( rect.Area() ) );
        for ( uint32_t z = rect.Z0; z < rect.Z1; ++z )
        {
            const auto row = m_Samples.begin() + static_cast<ptrdiff_t>( static_cast<size_t>( z ) * m_SamplesX );
            out.insert( out.end(), row + rect.X0, row + rect.X1 );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr LandscapeTileData::WriteRegion( const LandscapeRect& rect, std::span<const uint16_t> values )
    {
        if ( auto valid = ValidateRect( rect, m_SamplesX, m_SamplesZ ); !valid )
            return valid;
        if ( values.size() != rect.Area() )
            return Common::MakeFormattedError<bool>( "Landscape region {} x {} needs {} values, got {}", rect.Width(),
                                                     rect.Depth(), rect.Area(), values.size() );
        bool changed = false;
        for ( uint32_t z = rect.Z0; z < rect.Z1; ++z )
        {
            uint16_t*       row = m_Samples.data() + static_cast<size_t>( z ) * m_SamplesX + rect.X0;
            const uint16_t* src = values.data() + static_cast<size_t>( z - rect.Z0 ) * rect.Width();
            for ( uint32_t i = 0; i < rect.Width(); ++i )
            {
                changed = changed || row[i] != src[i];
                row[i]  = src[i];
            }
        }
        if ( changed )
            MarkDirty( rect );
        return Common::MakeSuccess( true );
    }

    std::vector<LandscapeRect> LandscapeTileData::TakeDirtyRects()
    {
        return std::exchange( m_Dirty, {} );
    }

    void LandscapeTileData::MarkDirty( LandscapeRect rect )
    {
        // Absorbing one rectangle can make the grown one touch another that it did not touch before, so the
        // scan restarts until a pass absorbs nothing. The list stays tiny (disjoint, non-touching boxes on
        // one tile), so the quadratic worst case is a handful of comparisons.
        bool absorbed = true;
        while ( absorbed )
        {
            absorbed = false;
            for ( size_t i = 0; i < m_Dirty.size(); ++i )
            {
                if ( !Touches( m_Dirty[i], rect ) )
                    continue;
                rect = Union( m_Dirty[i], rect );
                m_Dirty.erase( m_Dirty.begin() + static_cast<ptrdiff_t>( i ) );
                absorbed = true;
                break;
            }
        }
        m_Dirty.push_back( rect );
    }

    // ── Sampling ──────────────────────────────────────────────────────────────────────────────────────

    namespace
    {
        // Where a world point falls on the tile's grid: the cell's lower-left sample and the fractions
        // across it. The far edge maps to the last cell at fraction 1, not to a cell that does not exist.
        struct GridPoint
        {
            uint32_t CellX = 0u;
            uint32_t CellZ = 0u;
            float    Fx    = 0.0f;
            float    Fz    = 0.0f;
        };

        std::optional<GridPoint> Locate( const LandscapeTileData& tile, const LandscapeFrame& frame, float worldX,
                                         float worldZ )
        {
            const float gx = ( worldX - frame.OriginX ) / frame.SpacingCm;
            const float gz = ( worldZ - frame.OriginZ ) / frame.SpacingCm;
            const auto  lastX = static_cast<float>( tile.SamplesX() - 1u );
            const auto  lastZ = static_cast<float>( tile.SamplesZ() - 1u );
            // Written as !(inside) so NaN lands outside rather than on sample 0.
            if ( !( gx >= 0.0f && gx <= lastX && gz >= 0.0f && gz <= lastZ ) )
                return std::nullopt;
            GridPoint p;
            p.CellX = std::min( static_cast<uint32_t>( gx ), tile.SamplesX() - 2u );
            p.CellZ = std::min( static_cast<uint32_t>( gz ), tile.SamplesZ() - 2u );
            p.Fx    = gx - static_cast<float>( p.CellX );
            p.Fz    = gz - static_cast<float>( p.CellZ );
            return p;
        }

        float Bilinear( float h00, float h10, float h01, float h11, float fx, float fz )
        {
            const float bottom = h00 + ( h10 - h00 ) * fx;
            const float top    = h01 + ( h11 - h01 ) * fx;
            return bottom + ( top - bottom ) * fz;
        }

        float HeightAt( const LandscapeTileData& tile, const LandscapeFrame& frame, uint32_t x, uint32_t z )
        {
            return LandscapeHeightCm( tile.Sample( x, z ), frame.ZScale );
        }

        // dh/dx and dh/dz at a sample, in cm per cm. Central where both neighbours exist; one-sided on the
        // border, because the missing neighbour lives in another tile this function cannot see.
        std::pair<float, float> GradientAt( const LandscapeTileData& tile, const LandscapeFrame& frame, uint32_t x,
                                            uint32_t z )
        {
            const uint32_t xa = x > 0u ? x - 1u : x;
            const uint32_t xb = x + 1u < tile.SamplesX() ? x + 1u : x;
            const uint32_t za = z > 0u ? z - 1u : z;
            const uint32_t zb = z + 1u < tile.SamplesZ() ? z + 1u : z;
            const float    dx = ( HeightAt( tile, frame, xb, z ) - HeightAt( tile, frame, xa, z ) ) /
                             ( static_cast<float>( xb - xa ) * frame.SpacingCm );
            const float dz = ( HeightAt( tile, frame, x, zb ) - HeightAt( tile, frame, x, za ) ) /
                             ( static_cast<float>( zb - za ) * frame.SpacingCm );
            return { dx, dz };
        }
    } // namespace

    std::optional<float> SampleLandscapeHeight( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                float worldX, float worldZ )
    {
        const auto p = Locate( tile, frame, worldX, worldZ );
        if ( !p )
            return std::nullopt;
        const float h = Bilinear( HeightAt( tile, frame, p->CellX, p->CellZ ),
                                  HeightAt( tile, frame, p->CellX + 1u, p->CellZ ),
                                  HeightAt( tile, frame, p->CellX, p->CellZ + 1u ),
                                  HeightAt( tile, frame, p->CellX + 1u, p->CellZ + 1u ), p->Fx, p->Fz );
        return frame.BaseY + h;
    }

    std::optional<glm::vec3> SampleLandscapeNormal( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                    float worldX, float worldZ )
    {
        const auto p = Locate( tile, frame, worldX, worldZ );
        if ( !p )
            return std::nullopt;
        const auto g00 = GradientAt( tile, frame, p->CellX, p->CellZ );
        const auto g10 = GradientAt( tile, frame, p->CellX + 1u, p->CellZ );
        const auto g01 = GradientAt( tile, frame, p->CellX, p->CellZ + 1u );
        const auto g11 = GradientAt( tile, frame, p->CellX + 1u, p->CellZ + 1u );
        const float dx = Bilinear( g00.first, g10.first, g01.first, g11.first, p->Fx, p->Fz );
        const float dz = Bilinear( g00.second, g10.second, g01.second, g11.second, p->Fx, p->Fz );
        // The surface y = h(x, z) has the (unnormalised) normal (-dh/dx, 1, -dh/dz).
        return glm::normalize( glm::vec3( -dx, 1.0f, -dz ) );
    }

    // ── The blob ──────────────────────────────────────────────────────────────────────────────────────

    std::vector<unsigned char> EncodeLandscapeTile( const LandscapeTileData& tile )
    {
        const std::vector<uint16_t>& samples      = tile.Samples();
        const uint64_t               payloadBytes = static_cast<uint64_t>( samples.size() ) * 2u;

        std::vector<unsigned char> out;
        out.reserve( kLandscapeTileHeaderSize + static_cast<size_t>( payloadBytes ) + kLandscapeTileTrailerSize );
        out.insert( out.end(), kLandscapeTileMagic, kLandscapeTileMagic + sizeof( kLandscapeTileMagic ) );
        Assets::WriteU32( out, kLandscapeTileContainerVersion );
        Assets::WriteU32( out, tile.SamplesX() );
        Assets::WriteU32( out, tile.SamplesZ() );
        Assets::WriteU32( out, 0u ); // edit layers — see kLandscapeMaxEditLayers
        Assets::WriteU64( out, payloadBytes );
        for ( const uint16_t s : samples )
        {
            out.push_back( static_cast<unsigned char>( s & 0xFFu ) );
            out.push_back( static_cast<unsigned char>( ( s >> 8 ) & 0xFFu ) );
        }
        Assets::WriteU32( out, Common::Utils::Crc32c( out.data(), out.size() ) );
        return out;
    }

    Common::ResultStr<LandscapeTileData> DecodeLandscapeTile( std::span<const unsigned char> bytes )
    {
        using Result = LandscapeTileData;
        if ( bytes.size() < kLandscapeTileHeaderSize + kLandscapeTileTrailerSize )
            return Common::MakeFormattedError<Result>( "Landscape tile blob of {} bytes is shorter than its {}-byte "
                                                       "header and trailer",
                                                       bytes.size(), kLandscapeTileHeaderSize + kLandscapeTileTrailerSize );
        const unsigned char* at = bytes.data();
        if ( std::memcmp( at, kLandscapeTileMagic, sizeof( kLandscapeTileMagic ) ) != 0 )
            return Common::MakeFormattedError<Result>( "Landscape tile blob has magic {:02x}{:02x}{:02x}{:02x}, "
                                                       "expected 'DLHT'",
                                                       at[0], at[1], at[2], at[3] );
        const uint32_t version = Assets::ReadU32( at + 4 );
        if ( version != kLandscapeTileContainerVersion )
            return Common::MakeFormattedError<Result>( "Landscape tile blob version {} is not the supported {}",
                                                       version, kLandscapeTileContainerVersion );

        // The checksum before any field is believed: a header bit flip that keeps every length consistent
        // is exactly what the checks below cannot see.
        const size_t   covered   = bytes.size() - kLandscapeTileTrailerSize;
        const uint32_t storedCrc = Assets::ReadU32( at + covered );
        const uint32_t actualCrc = Common::Utils::Crc32c( at, covered );
        if ( storedCrc != actualCrc )
            return Common::MakeFormattedError<Result>( "Landscape tile blob checksum {:08x} does not match its "
                                                       "contents' {:08x} ({} bytes)",
                                                       storedCrc, actualCrc, bytes.size() );

        const uint32_t samplesX = Assets::ReadU32( at + 8 );
        const uint32_t samplesZ = Assets::ReadU32( at + 12 );
        if ( auto valid = ValidateDimensions( samplesX, samplesZ ); !valid )
            return Common::MakeError<Result>( valid.GetError() );
        const uint32_t layers = Assets::ReadU32( at + 16 );
        if ( layers > kLandscapeMaxEditLayers )
            return Common::MakeFormattedError<Result>( "Landscape tile blob carries {} edit layers; this version "
                                                       "carries at most {} (Edit Layers are round two)",
                                                       layers, kLandscapeMaxEditLayers );
        const uint64_t payloadBytes = Assets::ReadU64( at + 20 );
        const uint64_t expected     = static_cast<uint64_t>( samplesX ) * samplesZ * 2u;
        if ( payloadBytes != expected )
            return Common::MakeFormattedError<Result>( "Landscape tile blob of {} x {} states {} payload bytes, "
                                                       "the dimensions need {}",
                                                       samplesX, samplesZ, payloadBytes, expected );
        if ( covered != kLandscapeTileHeaderSize + expected )
            return Common::MakeFormattedError<Result>( "Landscape tile blob is {} bytes, header + payload + trailer "
                                                       "is {}",
                                                       bytes.size(),
                                                       kLandscapeTileHeaderSize + expected + kLandscapeTileTrailerSize );

        const unsigned char*  payload = at + kLandscapeTileHeaderSize;
        std::vector<uint16_t> samples( static_cast<size_t>( samplesX ) * samplesZ );
        for ( size_t i = 0; i < samples.size(); ++i )
            samples[i] = static_cast<uint16_t>( payload[2u * i] | ( payload[2u * i + 1u] << 8 ) );
        return LandscapeTileData::FromSamples( samplesX, samplesZ, std::move( samples ) );
    }
} // namespace Desert::World::Landscape
