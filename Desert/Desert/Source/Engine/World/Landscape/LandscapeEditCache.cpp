// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModeTools.h:283-745 and
// Engine/Source/Runtime/Landscape/Public/LandscapeEdit.h:361-480, adapted: see LandscapeEditCache.hpp.

#include <Engine/World/Landscape/LandscapeEditCache.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace Desert::World::Landscape
{
    namespace
    {
        int32_t FloorDiv( int32_t value, int32_t divisor )
        {
            const int32_t q = value / divisor;
            return ( value % divisor != 0 && ( value < 0 ) != ( divisor < 0 ) ) ? q - 1 : q;
        }

        std::string TileName( int32_t tileX, int32_t tileZ )
        {
            return "(" + std::to_string( tileX ) + ", " + std::to_string( tileZ ) + ")";
        }

        std::string RectName( int32_t x1, int32_t z1, int32_t x2, int32_t z2 )
        {
            return "[" + std::to_string( x1 ) + ".." + std::to_string( x2 ) + "] x [" + std::to_string( z1 ) +
                   ".." + std::to_string( z2 ) + "]";
        }

        LandscapeRect Union( const LandscapeRect& a, const LandscapeRect& b )
        {
            return { std::min( a.X0, b.X0 ), std::min( a.Z0, b.Z0 ), std::max( a.X1, b.X1 ),
                     std::max( a.Z1, b.Z1 ) };
        }
    } // namespace

    LandscapeHeightCache::LandscapeHeightCache( const LandscapeRoot& root, LandscapeTileLookup lookup )
         : m_Root( root ), m_Lookup( std::move( lookup ) )
    {
    }

    std::vector<LandscapeHeightCache::TileSpan> LandscapeHeightCache::TilesOf( int32_t x1, int32_t z1, int32_t x2,
                                                                               int32_t z2 ) const
    {
        // Tile t stores global samples t·Q .. t·Q + Q inclusive, so sample g is in tiles floor((g-1)/Q) ..
        // floor(g/Q) — UE's GetComponentsInRegion, which also takes the component whose edge only touches the
        // region.
        const int32_t         q = static_cast<int32_t>( m_Root.QuadsPerTile );
        std::vector<TileSpan> spans;
        for ( int32_t tz = FloorDiv( z1 - 1, q ); tz <= FloorDiv( z2, q ); ++tz )
            for ( int32_t tx = FloorDiv( x1 - 1, q ); tx <= FloorDiv( x2, q ); ++tx )
            {
                const int32_t bx = tx * q;
                const int32_t bz = tz * q;
                LandscapeRect local;
                local.X0 = static_cast<uint32_t>( std::max( x1, bx ) - bx );
                local.Z0 = static_cast<uint32_t>( std::max( z1, bz ) - bz );
                local.X1 = static_cast<uint32_t>( std::min( x2, bx + q ) - bx + 1 );
                local.Z1 = static_cast<uint32_t>( std::min( z2, bz + q ) - bz + 1 );
                if ( !local.Empty() )
                    spans.push_back( { tx, tz, local } );
            }
        return spans;
    }

    Common::BoolResultStr LandscapeHeightCache::ReadInto( int32_t x1, int32_t z1, int32_t x2, int32_t z2,
                                                          int32_t cx1, int32_t cz1, int32_t cx2,
                                                          std::vector<uint16_t>& cache ) const
    {
        const int32_t     q      = static_cast<int32_t>( m_Root.QuadsPerTile );
        const int32_t     stride = cx2 - cx1 + 1;
        const int32_t     width  = x2 - x1 + 1;
        std::vector<bool> read( static_cast<size_t>( width ) * static_cast<size_t>( z2 - z1 + 1 ), false );
        for ( const TileSpan& span : TilesOf( x1, z1, x2, z2 ) )
        {
            const LandscapeTileSlot slot = m_Lookup( span.TileX, span.TileZ );
            if ( slot.State != LandscapeTileState::Present )
                continue;
            if ( auto match = CheckTileMatchesRoot( *slot.Data, m_Root ); !match.IsSuccess() )
                return Common::MakeError( "landscape edit cache: tile " + TileName( span.TileX, span.TileZ ) +
                                          ": " + match.GetError() );
            for ( uint32_t lz = span.Local.Z0; lz < span.Local.Z1; ++lz )
                for ( uint32_t lx = span.Local.X0; lx < span.Local.X1; ++lx )
                {
                    const int32_t gx = span.TileX * q + static_cast<int32_t>( lx );
                    const int32_t gz = span.TileZ * q + static_cast<int32_t>( lz );
                    cache[static_cast<size_t>( ( gz - cz1 ) * stride + ( gx - cx1 ) )] =
                         slot.Data->Sample( lx, lz );
                    read[static_cast<size_t>( ( gz - z1 ) * width + ( gx - x1 ) )] = true;
                }
        }
        for ( size_t i = 0; i < read.size(); ++i )
            if ( !read[i] )
            {
                const int32_t gx = x1 + static_cast<int32_t>( i % static_cast<size_t>( width ) );
                const int32_t gz = z1 + static_cast<int32_t>( i / static_cast<size_t>( width ) );
                return Common::MakeError( "landscape edit cache: sample (" + std::to_string( gx ) + ", " +
                                          std::to_string( gz ) + ") is stored by no loaded tile" );
            }
        return Common::MakeSuccess( true );
    }

    bool LandscapeHeightCache::Covers( int32_t x1, int32_t z1, int32_t x2, int32_t z2 ) const
    {
        return m_Valid && x1 >= m_X1 && z1 >= m_Z1 && x2 <= m_X2 && z2 <= m_Z2;
    }

    Common::BoolResultStr LandscapeHeightCache::CacheData( int32_t x1, int32_t z1, int32_t x2, int32_t z2 )
    {
        if ( x1 > x2 || z1 > z2 )
            return Common::MakeError( "landscape edit cache: inverted rectangle " + RectName( x1, z1, x2, z2 ) );
        if ( Covers( x1, z1, x2, z2 ) )
            return Common::MakeSuccess( true );

        const int32_t nx1 = m_Valid ? std::min( x1, m_X1 ) : x1;
        const int32_t nz1 = m_Valid ? std::min( z1, m_Z1 ) : z1;
        const int32_t nx2 = m_Valid ? std::max( x2, m_X2 ) : x2;
        const int32_t nz2 = m_Valid ? std::max( z2, m_Z2 ) : z2;

        std::vector<uint16_t> grown( static_cast<size_t>( nx2 - nx1 + 1 ) * static_cast<size_t>( nz2 - nz1 + 1 ) );
        if ( auto read = ReadInto( nx1, nz1, nx2, nz2, nx1, nz1, nx2, grown ); !read.IsSuccess() )
            return read;
        // The whole box was read above for simplicity; the already-cached region is then put back from the cache,
        // which is what UE's strip-only extension gives.
        if ( m_Valid )
            for ( int32_t z = m_Z1; z <= m_Z2; ++z )
                std::copy_n( m_Data.begin() + static_cast<std::ptrdiff_t>( ( z - m_Z1 ) * ( m_X2 - m_X1 + 1 ) ),
                             m_X2 - m_X1 + 1,
                             grown.begin() + static_cast<std::ptrdiff_t>( ( z - nz1 ) * ( nx2 - nx1 + 1 ) +
                                                                          ( m_X1 - nx1 ) ) );
        m_Data  = std::move( grown );
        m_X1    = nx1;
        m_Z1    = nz1;
        m_X2    = nx2;
        m_Z2    = nz2;
        m_Valid = true;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::vector<uint16_t>> LandscapeHeightCache::GetCachedData( int32_t x1, int32_t z1,
                                                                                  int32_t x2, int32_t z2 ) const
    {
        if ( x1 > x2 || z1 > z2 )
            return Common::MakeError<std::vector<uint16_t>>( "landscape edit cache: inverted rectangle " +
                                                             RectName( x1, z1, x2, z2 ) );
        if ( !Covers( x1, z1, x2, z2 ) )
            return Common::MakeError<std::vector<uint16_t>>(
                 "landscape edit cache: " + RectName( x1, z1, x2, z2 ) + " is not inside the cached region " +
                 RectName( m_X1, m_Z1, m_X2, m_Z2 ) );
        const int32_t         width = x2 - x1 + 1;
        std::vector<uint16_t> out( static_cast<size_t>( width ) * static_cast<size_t>( z2 - z1 + 1 ) );
        for ( int32_t z = z1; z <= z2; ++z )
            std::copy_n( m_Data.begin() +
                              static_cast<std::ptrdiff_t>( ( z - m_Z1 ) * ( m_X2 - m_X1 + 1 ) + ( x1 - m_X1 ) ),
                         width, out.begin() + static_cast<std::ptrdiff_t>( ( z - z1 ) * width ) );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr LandscapeHeightCache::SetCachedData( int32_t x1, int32_t z1, int32_t x2, int32_t z2,
                                                               std::span<const uint16_t> values )
    {
        if ( x1 > x2 || z1 > z2 )
            return Common::MakeError( "landscape edit cache: inverted rectangle " + RectName( x1, z1, x2, z2 ) );
        if ( !Covers( x1, z1, x2, z2 ) )
            return Common::MakeError( "landscape edit cache: " + RectName( x1, z1, x2, z2 ) +
                                      " is not inside the cached region " + RectName( m_X1, m_Z1, m_X2, m_Z2 ) );
        const int32_t width = x2 - x1 + 1;
        const size_t  count = static_cast<size_t>( width ) * static_cast<size_t>( z2 - z1 + 1 );
        if ( values.size() != count )
            return Common::MakeError( "landscape edit cache: " + std::to_string( values.size() ) + " values for " +
                                      std::to_string( count ) + " samples" );

        // Validate everything before writing anything: a stroke half-applied would leave a seam whose two
        // copies disagree, the one outcome this cache exists to prevent.
        std::vector<std::pair<TileSpan, LandscapeTileData*>> targets;
        std::vector<bool>                                    written( count, false );
        const int32_t                                        q = static_cast<int32_t>( m_Root.QuadsPerTile );
        for ( const TileSpan& span : TilesOf( x1, z1, x2, z2 ) )
        {
            const LandscapeTileSlot slot = m_Lookup( span.TileX, span.TileZ );
            if ( slot.State == LandscapeTileState::Unloaded )
                return Common::MakeError( "landscape edit cache: refusing to write " + RectName( x1, z1, x2, z2 ) +
                                          ": tile " + TileName( span.TileX, span.TileZ ) +
                                          " shares these samples and is not loaded" );
            if ( slot.State == LandscapeTileState::Absent )
                continue;
            if ( auto match = CheckTileMatchesRoot( *slot.Data, m_Root ); !match.IsSuccess() )
                return Common::MakeError( "landscape edit cache: tile " + TileName( span.TileX, span.TileZ ) +
                                          ": " + match.GetError() );
            for ( uint32_t lz = span.Local.Z0; lz < span.Local.Z1; ++lz )
                for ( uint32_t lx = span.Local.X0; lx < span.Local.X1; ++lx )
                    written[static_cast<size_t>( ( span.TileZ * q + static_cast<int32_t>( lz ) - z1 ) * width +
                                                 ( span.TileX * q + static_cast<int32_t>( lx ) - x1 ) )] = true;
            targets.emplace_back( span, slot.Data );
        }
        for ( size_t i = 0; i < count; ++i )
            if ( !written[i] )
                return Common::MakeError(
                     "landscape edit cache: refusing to write " + RectName( x1, z1, x2, z2 ) + ": sample (" +
                     std::to_string( x1 + static_cast<int32_t>( i % static_cast<size_t>( width ) ) ) + ", " +
                     std::to_string( z1 + static_cast<int32_t>( i / static_cast<size_t>( width ) ) ) +
                     ") is stored by no tile" );

        for ( int32_t z = z1; z <= z2; ++z )
            std::copy_n( values.begin() + static_cast<std::ptrdiff_t>( ( z - z1 ) * width ), width,
                         m_Data.begin() +
                              static_cast<std::ptrdiff_t>( ( z - m_Z1 ) * ( m_X2 - m_X1 + 1 ) + ( x1 - m_X1 ) ) );

        // The same global sample goes to every tile that stores it, from the one value above.
        for ( const auto& [span, tile] : targets )
        {
            std::vector<uint16_t> local;
            local.reserve( span.Local.Area() );
            for ( uint32_t lz = span.Local.Z0; lz < span.Local.Z1; ++lz )
                for ( uint32_t lx = span.Local.X0; lx < span.Local.X1; ++lx )
                    local.push_back(
                         values[static_cast<size_t>( ( span.TileZ * q + static_cast<int32_t>( lz ) - z1 ) * width +
                                                     ( span.TileX * q + static_cast<int32_t>( lx ) - x1 ) )] );
            auto write = tile->WriteRegion( span.Local, local );
            // Every precondition WriteRegion checks was checked above; a refusal here is a defect in this file.
            if ( !write.IsSuccess() )
                return Common::MakeError( "landscape edit cache: internal: tile " +
                                          TileName( span.TileX, span.TileZ ) +
                                          " refused a validated write: " + write.GetError() );
            RecordChanged( span.TileX, span.TileZ, span.Local );
        }
        return Common::MakeSuccess( true );
    }

    void LandscapeHeightCache::RecordChanged( int32_t tileX, int32_t tileZ, const LandscapeRect& local )
    {
        for ( LandscapeChangedTile& changed : m_Changed )
            if ( changed.TileX == tileX && changed.TileZ == tileZ )
            {
                changed.Samples = Union( changed.Samples, local );
                return;
            }
        m_Changed.push_back( { tileX, tileZ, local } );
    }

    std::vector<LandscapeChangedTile> LandscapeHeightCache::TakeChangedTiles()
    {
        return std::exchange( m_Changed, {} );
    }
} // namespace Desert::World::Landscape
