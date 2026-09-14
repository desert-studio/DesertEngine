#include "CloudNoiseVolumeSheet.hpp"

#include <algorithm>
#include <string>

namespace Desert::Assets
{
    namespace
    {
        constexpr uint32_t kBytesPerVoxel = 4u;

        // The legal resolutions, enumerated rather than solved for. The importer has to answer "which cube,
        // if any, does an image of this size hold", and the honest way to ask that is to try every cube the
        // container would accept — there are two — and compare exactly. Deriving N from a cube root instead
        // would introduce a rounding decision into a question that has none, and the failure it produces is
        // an off-by-one volume that decodes, uploads and renders as a subtly wrong sky.
        std::vector<uint32_t> LegalResolutions()
        {
            std::vector<uint32_t> out;
            for ( uint32_t r = 1u; r <= 4096u; r <<= 1 )
            {
                if ( ValidateCloudNoiseVolumeResolution( r ) )
                    out.push_back( r );
            }
            return out;
        }
    } // namespace

    Common::ResultStr<CloudNoiseSheetLayout> CloudNoiseSheetLayoutFor( uint32_t resolution )
    {
        // The container's own check, called rather than restated. A sheet must not be a second opinion about
        // what size a volume may be, or one of the two will eventually accept what the other refuses.
        if ( auto valid = ValidateCloudNoiseVolumeResolution( resolution ); !valid )
            return Common::MakeFormattedError<CloudNoiseSheetLayout>( "{}", valid.GetError() );

        // Smallest power of two whose square is at least N. Both axes come out powers of two because N is,
        // which keeps the sheet a texture-shaped image in whatever tool the artist opens it in.
        uint32_t tilesX = 1u;
        while ( tilesX * tilesX < resolution )
            tilesX <<= 1;

        CloudNoiseSheetLayout layout;
        layout.Resolution = resolution;
        layout.TilesX     = tilesX;
        layout.TilesY     = resolution / tilesX;
        layout.Width      = layout.TilesX * resolution;
        layout.Height     = layout.TilesY * resolution;

        // The grid must hold every slice and no more. This cannot fail for a power-of-two N, and it is
        // asserted anyway because it is the one relation the whole file rests on: a grid one tile short
        // would drop the last slice and a grid one tile long would import uninitialised pixels as cloud.
        if ( layout.TilesX * layout.TilesY != resolution )
            return Common::MakeFormattedError<CloudNoiseSheetLayout>(
                 "a {}x{} tile grid holds {} slices, but a {}^3 volume has {}", layout.TilesX, layout.TilesY,
                 layout.TilesX * layout.TilesY, resolution, resolution );

        return Common::MakeSuccess( layout );
    }

    std::string CloudNoiseSheetSizesDescription()
    {
        std::string out;
        for ( const uint32_t resolution : LegalResolutions() )
        {
            const auto layout = CloudNoiseSheetLayoutFor( resolution );
            if ( !layout )
                continue;

            if ( !out.empty() )
                out += ", ";
            out += std::to_string( layout.GetValue().Width ) + "x" + std::to_string( layout.GetValue().Height ) +
                   " (" + std::to_string( resolution ) + "^3)";
        }
        return out;
    }

    Common::ResultStr<CloudNoiseSheetImage> EncodeCloudNoiseVolumeToSheet( const CloudNoiseVolumeData& volume )
    {
        auto layout = CloudNoiseSheetLayoutFor( volume.Params.Resolution );
        if ( !layout )
            return Common::MakeFormattedError<CloudNoiseSheetImage>( "cannot lay this volume out flat: {}",
                                                                     layout.GetError() );

        const CloudNoiseSheetLayout sheet    = layout.GetValue();
        const uint64_t              expected = volume.VoxelCount() * kBytesPerVoxel;
        if ( volume.Voxels.size() != expected )
            return Common::MakeFormattedError<CloudNoiseSheetImage>(
                 "volume has {} voxel bytes but a {}^3 RGBA8 volume is {}", volume.Voxels.size(),
                 volume.Params.Resolution, expected );

        const uint32_t n = sheet.Resolution;

        CloudNoiseSheetImage image;
        image.Layout = sheet;
        image.Pixels.assign( static_cast<size_t>( sheet.Width ) * sheet.Height * kBytesPerVoxel, 0u );

        for ( uint32_t z = 0; z < n; ++z )
        {
            const uint32_t tileX   = z % sheet.TilesX;
            const uint32_t tileY   = z / sheet.TilesX;
            const uint32_t originX = tileX * n;
            const uint32_t originY = tileY * n;

            for ( uint32_t y = 0; y < n; ++y )
            {
                // One row of one slice is contiguous in BOTH layouts, so the whole inner loop over x is a
                // single copy. That is not an optimisation for its own sake: it is what makes the mapping
                // between the two layouts one expression instead of four index computations that could
                // each be wrong independently.
                const size_t source = ( ( static_cast<size_t>( z ) * n + y ) * n ) * kBytesPerVoxel;
                const size_t target =
                     ( static_cast<size_t>( originY + y ) * sheet.Width + originX ) * kBytesPerVoxel;

                std::copy( volume.Voxels.begin() + static_cast<std::ptrdiff_t>( source ),
                           volume.Voxels.begin() +
                                static_cast<std::ptrdiff_t>( source + static_cast<size_t>( n ) * kBytesPerVoxel ),
                           image.Pixels.begin() + static_cast<std::ptrdiff_t>( target ) );
            }
        }

        return Common::MakeSuccess( std::move( image ) );
    }

    Common::ResultStr<CloudNoiseVolumeData>
    DecodeCloudNoiseVolumeFromSheet( const std::vector<unsigned char>& pixels, uint32_t width, uint32_t height )
    {
        const size_t needed = static_cast<size_t>( width ) * height * kBytesPerVoxel;
        if ( pixels.size() != needed )
            return Common::MakeFormattedError<CloudNoiseVolumeData>(
                 "image is {}x{}, which is {} RGBA8 bytes, but {} were handed over", width, height, needed,
                 pixels.size() );

        // WHICH CUBE, IF ANY. Every legal resolution is tried and compared exactly; nothing is scaled,
        // cropped or padded to make one fit. The refusal names the size that arrived AND the sizes that
        // would have worked, because a rejected import that only says "no" sends the artist back to guess.
        CloudNoiseSheetLayout match;
        bool                  matched = false;
        for ( const uint32_t resolution : LegalResolutions() )
        {
            const auto layout = CloudNoiseSheetLayoutFor( resolution );
            if ( layout && layout.GetValue().Width == width && layout.GetValue().Height == height )
            {
                match   = layout.GetValue();
                matched = true;
                break;
            }
        }

        if ( !matched )
            return Common::MakeFormattedError<CloudNoiseVolumeData>(
                 "a {}x{} image is not a tiled slice sheet of any volume this build accepts. The sheet sizes "
                 "are {}. Nothing was resampled: a volume stretched to fit would be a cloud shape you could "
                 "not trace back to what you exported",
                 width, height, CloudNoiseSheetSizesDescription() );

        const uint32_t n = match.Resolution;

        CloudNoiseVolumeData volume;
        volume.Params = EmptyImportedRecipe( n );
        volume.Origin = CloudNoiseVolumeOrigin::Imported;

        // The generator version records which MATHS produced a volume, and no version of our maths produced
        // this one. Zero says exactly that, and it is why the field is read rather than assumed on load.
        volume.GeneratorVersion = 0u;

        volume.Voxels.assign( static_cast<size_t>( n ) * n * n * kBytesPerVoxel, 0u );

        for ( uint32_t z = 0; z < n; ++z )
        {
            const uint32_t originX = ( z % match.TilesX ) * n;
            const uint32_t originY = ( z / match.TilesX ) * n;

            for ( uint32_t y = 0; y < n; ++y )
            {
                const size_t source =
                     ( static_cast<size_t>( originY + y ) * match.Width + originX ) * kBytesPerVoxel;
                const size_t target = ( ( static_cast<size_t>( z ) * n + y ) * n ) * kBytesPerVoxel;

                std::copy( pixels.begin() + static_cast<std::ptrdiff_t>( source ),
                           pixels.begin() +
                                static_cast<std::ptrdiff_t>( source + static_cast<size_t>( n ) * kBytesPerVoxel ),
                           volume.Voxels.begin() + static_cast<std::ptrdiff_t>( target ) );
            }
        }

        return Common::MakeSuccess( std::move( volume ) );
    }
} // namespace Desert::Assets
