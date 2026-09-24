#include <Engine/World/Landscape/LandscapeData.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/GlslAsCpp.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Engine/Assets/ContainerBytes.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Desert::World::Landscape
{
    namespace
    {
        // Shaders/Common/LandscapeHeight.glslh COMPILED AS C++ — the same text Terrain.shader decodes a
        // tile's R16 copy with, so the CPU's heights and the drawn heights are one function. The shader
        // root is on the include path of every project that compiles this file.
        using glm::floor;
        using glm::min;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/LandscapeHeight.glslh>
             DESERT_GLSL_AS_CPP_END
    } // namespace

    float LandscapeLocalHeight( uint16_t sample )
    {
        return LandscapeLocalFromSample( static_cast<float>( sample ) );
    }

    float LandscapeHeightCm( uint16_t sample, float zScale )
    {
        return LandscapeHeightCmFromSample( static_cast<float>( sample ), zScale );
    }

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
                return Common::MakeFormattedError<bool>(
                     "Landscape tile of {} x {} samples: each side must lie in "
                     "[{}, {}]",
                     samplesX, samplesZ, kLandscapeMinTileSamples, kLandscapeMaxTileSamples );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr ValidateRect( const LandscapeRect& rect, uint32_t samplesX, uint32_t samplesZ )
        {
            if ( rect.Empty() )
                return Common::MakeFormattedError<bool>( "Landscape region [{}, {}) x [{}, {}) is empty", rect.X0,
                                                         rect.X1, rect.Z0, rect.Z1 );
            if ( rect.X1 > samplesX || rect.Z1 > samplesZ )
                return Common::MakeFormattedError<bool>(
                     "Landscape region [{}, {}) x [{}, {}) leaves a tile of {} "
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
            return Common::MakeFormattedError<LandscapeTileData>(
                 "Landscape tile of {} x {} needs {} samples, got {}", samplesX, samplesZ, expected,
                 samples.size() );
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

    Common::BoolResultStr LandscapeTileData::WriteRegion( const LandscapeRect&      rect,
                                                          std::span<const uint16_t> values )
    {
        if ( auto valid = ValidateRect( rect, m_SamplesX, m_SamplesZ ); !valid )
            return valid;
        if ( values.size() != rect.Area() )
            return Common::MakeFormattedError<bool>( "Landscape region {} x {} needs {} values, got {}",
                                                     rect.Width(), rect.Depth(), rect.Area(), values.size() );
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

    std::vector<LandscapeRect> LandscapeTileData::TakeDirtyRects( LandscapeDirtyConsumer consumer )
    {
        return std::exchange( m_Dirty[static_cast<size_t>( consumer )], {} );
    }

    void LandscapeTileData::MarkDirty( LandscapeRect rect )
    {
        // Absorbing one rectangle can make the grown one touch another that it did not touch before, so the
        // scan restarts until a pass absorbs nothing. The list stays tiny (disjoint, non-touching boxes on
        // one tile), so the quadratic worst case is a handful of comparisons.
        for ( auto& list : m_Dirty )
        {
            LandscapeRect grown    = rect;
            bool          absorbed = true;
            while ( absorbed )
            {
                absorbed = false;
                for ( size_t i = 0; i < list.size(); ++i )
                {
                    if ( !Touches( list[i], grown ) )
                        continue;
                    grown = Union( list[i], grown );
                    list.erase( list.begin() + static_cast<ptrdiff_t>( i ) );
                    absorbed = true;
                    break;
                }
            }
            list.push_back( grown );
        }
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
            if ( tile.SamplesX() < kLandscapeMinTileSamples || tile.SamplesZ() < kLandscapeMinTileSamples )
                return std::nullopt; // the empty tile: covers nothing
            const float gx    = ( worldX - frame.OriginX ) / frame.SpacingCm;
            const float gz    = ( worldZ - frame.OriginZ ) / frame.SpacingCm;
            const auto  lastX = static_cast<float>( tile.SamplesX() - 1u );
            const auto  lastZ = static_cast<float>( tile.SamplesZ() - 1u );
            // Written as !(inside) so NaN lands outside rather than on sample 0.
            if ( !( gx >= 0.0f && gx <= lastX && gz >= 0.0f && gz <= lastZ ) )
                return std::nullopt;
            const float cellX = LandscapeCellOf( gx, static_cast<float>( tile.SamplesX() ) );
            const float cellZ = LandscapeCellOf( gz, static_cast<float>( tile.SamplesZ() ) );
            GridPoint   p;
            p.CellX = static_cast<uint32_t>( cellX );
            p.CellZ = static_cast<uint32_t>( cellZ );
            p.Fx    = gx - cellX;
            p.Fz    = gz - cellZ;
            return p;
        }

        float Bilinear( float h00, float h10, float h01, float h11, float fx, float fz )
        {
            return LandscapeBilinear( h00, h10, h01, h11, fx, fz );
        }

        float HeightAt( const LandscapeTileData& tile, const LandscapeFrame& frame, uint32_t x, uint32_t z )
        {
            return LandscapeHeightCm( tile.Sample( x, z ), frame.ZScale );
        }

        // A sample of the bordered grid: x in [-1, SamplesX], z in [-1, SamplesZ]. The ring row is the
        // neighbour's SECOND row (index 1 or Samples - 2): its first is this tile's own edge.
        uint16_t BorderedSample( const LandscapeTileData& tile, const LandscapeTileNeighbours& n, int32_t x,
                                 int32_t z )
        {
            const auto sx   = static_cast<int32_t>( tile.SamplesX() );
            const auto sz   = static_cast<int32_t>( tile.SamplesZ() );
            const bool outX = x < 0 || x >= sx;
            const bool outZ = z < 0 || z >= sz;
            const auto cx   = static_cast<uint32_t>( std::clamp( x, 0, sx - 1 ) );
            const auto cz   = static_cast<uint32_t>( std::clamp( z, 0, sz - 1 ) );
            if ( outX == outZ ) // inside, or a ring corner no gradient reads
                return tile.Sample( cx, cz );
            if ( x < 0 && n.West )
                return n.West->Sample( static_cast<uint32_t>( sx - 2 ), cz );
            if ( x >= sx && n.East )
                return n.East->Sample( 1u, cz );
            if ( z < 0 && n.South )
                return n.South->Sample( cx, static_cast<uint32_t>( sz - 2 ) );
            if ( z >= sz && n.North )
                return n.North->Sample( cx, 1u );
            return tile.Sample( cx, cz ); // absent neighbour: never differenced (the mask says so)
        }

        void VerifyNeighbours( const LandscapeTileData& tile, const LandscapeTileNeighbours& n )
        {
            for ( const LandscapeTileData* other : { n.West, n.East, n.South, n.North } )
                DESERT_VERIFY(
                     !other || ( other->SamplesX() == tile.SamplesX() && other->SamplesZ() == tile.SamplesZ() ),
                     "Landscape neighbour of {} x {} samples beside a tile of {} x {}",
                     other ? other->SamplesX() : 0u, other ? other->SamplesZ() : 0u, tile.SamplesX(),
                     tile.SamplesZ() );
        }

        float BorderedHeight( const LandscapeTileData& tile, const LandscapeTileNeighbours& n,
                              const LandscapeFrame& frame, float x, float z )
        {
            return LandscapeHeightCm(
                 BorderedSample( tile, n, static_cast<int32_t>( x ), static_cast<int32_t>( z ) ), frame.ZScale );
        }

        // dh/dx and dh/dz at a sample, in cm per cm — LandscapeGradientLow/High pick the two samples, the
        // same functions the terrain shader calls on the same bordered grid.
        std::pair<float, float> GradientAt( const LandscapeTileData& tile, const LandscapeTileNeighbours& n,
                                            const LandscapeFrame& frame, uint32_t ix, uint32_t iz )
        {
            const float x     = static_cast<float>( ix );
            const float z     = static_cast<float>( iz );
            const float lastX = static_cast<float>( tile.SamplesX() - 1u );
            const float lastZ = static_cast<float>( tile.SamplesZ() - 1u );
            const float xa    = LandscapeGradientLow( x, n.West ? 1.0f : 0.0f );
            const float xb    = LandscapeGradientHigh( x, lastX, n.East ? 1.0f : 0.0f );
            const float za    = LandscapeGradientLow( z, n.South ? 1.0f : 0.0f );
            const float zb    = LandscapeGradientHigh( z, lastZ, n.North ? 1.0f : 0.0f );
            const float dx =
                 LandscapeGradient( BorderedHeight( tile, n, frame, xa, z ),
                                    BorderedHeight( tile, n, frame, xb, z ), xb - xa, frame.SpacingCm );
            const float dz =
                 LandscapeGradient( BorderedHeight( tile, n, frame, x, za ),
                                    BorderedHeight( tile, n, frame, x, zb ), zb - za, frame.SpacingCm );
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

    uint32_t LandscapeNeighbourMask( const LandscapeTileNeighbours& neighbours )
    {
        return ( neighbours.West ? 1u : 0u ) | ( neighbours.East ? 2u : 0u ) | ( neighbours.South ? 4u : 0u ) |
               ( neighbours.North ? 8u : 0u );
    }

    std::vector<uint16_t> LandscapeBorderedSamples( const LandscapeTileData&       tile,
                                                    const LandscapeTileNeighbours& neighbours )
    {
        VerifyNeighbours( tile, neighbours );
        const auto            sx = static_cast<int32_t>( tile.SamplesX() );
        const auto            sz = static_cast<int32_t>( tile.SamplesZ() );
        std::vector<uint16_t> out;
        out.reserve( static_cast<size_t>( sx + 2 ) * static_cast<size_t>( sz + 2 ) );
        for ( int32_t z = -1; z <= sz; ++z )
            for ( int32_t x = -1; x <= sx; ++x )
                out.push_back( BorderedSample( tile, neighbours, x, z ) );
        return out;
    }

    std::optional<glm::vec3> SampleLandscapeNormal( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                    const LandscapeTileNeighbours& neighbours, float worldX,
                                                    float worldZ )
    {
        const auto p = Locate( tile, frame, worldX, worldZ );
        if ( !p )
            return std::nullopt;
        VerifyNeighbours( tile, neighbours );
        const auto  g00 = GradientAt( tile, neighbours, frame, p->CellX, p->CellZ );
        const auto  g10 = GradientAt( tile, neighbours, frame, p->CellX + 1u, p->CellZ );
        const auto  g01 = GradientAt( tile, neighbours, frame, p->CellX, p->CellZ + 1u );
        const auto  g11 = GradientAt( tile, neighbours, frame, p->CellX + 1u, p->CellZ + 1u );
        const float dx  = Bilinear( g00.first, g10.first, g01.first, g11.first, p->Fx, p->Fz );
        const float dz  = Bilinear( g00.second, g10.second, g01.second, g11.second, p->Fx, p->Fz );
        // The surface y = h(x, z) has the (unnormalised) normal (-dh/dx, 1, -dh/dz).
        return glm::normalize( glm::vec3( -dx, 1.0f, -dz ) );
    }

    // ── The blob ──────────────────────────────────────────────────────────────────────────────────────

    std::vector<unsigned char> EncodeLandscapeTile( const LandscapeTileData& tile )
    {
        DESERT_VERIFY( tile.SamplesX() >= kLandscapeMinTileSamples && tile.SamplesZ() >= kLandscapeMinTileSamples,
                       "Encoding an empty landscape tile ({} x {})", tile.SamplesX(), tile.SamplesZ() );
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
            return Common::MakeFormattedError<Result>(
                 "Landscape tile blob of {} bytes is shorter than its {}-byte "
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
            return Common::MakeFormattedError<Result>(
                 "Landscape tile blob is {} bytes, header + payload + trailer "
                 "is {}",
                 bytes.size(), kLandscapeTileHeaderSize + expected + kLandscapeTileTrailerSize );

        const unsigned char*  payload = at + kLandscapeTileHeaderSize;
        std::vector<uint16_t> samples( static_cast<size_t>( samplesX ) * samplesZ );
        for ( size_t i = 0; i < samples.size(); ++i )
            samples[i] = static_cast<uint16_t>( payload[2u * i] | ( payload[2u * i + 1u] << 8 ) );
        return LandscapeTileData::FromSamples( samplesX, samplesZ, std::move( samples ) );
    }
} // namespace Desert::World::Landscape
