#include <Engine/World/Landscape/LandscapeLayout.hpp>

#include <cmath>

namespace Desert::World::Landscape
{
    Common::BoolResultStr ValidateLandscapeRoot( const LandscapeRoot& root )
    {
        bool offered = false;
        for ( const uint32_t quads : kLandscapeTileQuadsValues )
            offered = offered || quads == root.QuadsPerTile;
        if ( !offered )
            return Common::MakeFormattedError<bool>(
                 "Landscape tile size must be one of UE's section sizes (7, 15, 31, 63, 127 or 255 quads), got {}",
                 root.QuadsPerTile );

        LandscapeFrame frame;
        frame.OriginX   = root.Origin.x;
        frame.OriginZ   = root.Origin.z;
        frame.BaseY     = root.Origin.y;
        frame.SpacingCm = root.SpacingCm;
        frame.ZScale    = root.ZScale;
        return ValidateLandscapeFrame( frame );
    }

    LandscapeFrame LandscapeTileFrame( const LandscapeRoot& root, int32_t tileX, int32_t tileZ )
    {
        // In double and then rounded once: tile 4000 of a 255-quad, 1 m landscape is 1.02e8 cm out, where a
        // float product of two floats and an add would round twice.
        const double extent = static_cast<double>( root.QuadsPerTile ) * static_cast<double>( root.SpacingCm );

        LandscapeFrame frame;
        frame.OriginX   = static_cast<float>( static_cast<double>( root.Origin.x ) + extent * tileX );
        frame.OriginZ   = static_cast<float>( static_cast<double>( root.Origin.z ) + extent * tileZ );
        frame.BaseY     = root.Origin.y;
        frame.SpacingCm = root.SpacingCm;
        frame.ZScale    = root.ZScale;
        return frame;
    }

    LandscapeTileRect LandscapeTileBounds( const LandscapeRoot& root, int32_t tileX, int32_t tileZ )
    {
        // The far edge from the NEXT tile's origin, not from this origin plus an extent: the two are the
        // same number only when no rounding happens, and the shared edge must be ONE number for both tiles
        // or WorldPartition sees a sliver of overhang on every seam.
        const LandscapeFrame first = LandscapeTileFrame( root, tileX, tileZ );
        const LandscapeFrame next  = LandscapeTileFrame( root, tileX + 1, tileZ + 1 );

        LandscapeTileRect rect;
        rect.MinX = first.OriginX;
        rect.MinZ = first.OriginZ;
        rect.MaxX = next.OriginX;
        rect.MaxZ = next.OriginZ;
        return rect;
    }

    Common::BoolResultStr CheckTileMatchesRoot( const LandscapeTileData& tile, const LandscapeRoot& root )
    {
        const uint32_t expected = LandscapeTileSamples( root );
        if ( tile.SamplesX() != expected || tile.SamplesZ() != expected )
            return Common::MakeFormattedError<bool>( "Landscape tile is {} x {} samples but its landscape cuts "
                                                     "tiles of {} quads, i.e. {} x {} samples",
                                                     tile.SamplesX(), tile.SamplesZ(), root.QuadsPerTile, expected,
                                                     expected );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::World::Landscape
