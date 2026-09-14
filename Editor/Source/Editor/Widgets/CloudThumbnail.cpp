#include "CloudThumbnail.hpp"

#include <Editor/Widgets/ThumbnailFormats.hpp>

#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/UIThemeData.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

#include <Common/Core/Logger.hpp>

// STB_IMAGE_WRITE_IMPLEMENTATION is already compiled into Desert.lib (stb_image.obj); just declare here.
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Desert::Editor::CloudThumbnail
{
    namespace
    {
        constexpr uint32_t kSide  = kSize;
        constexpr size_t   kBytes = static_cast<size_t>( kSide ) * kSide * 4u;

        // THE PALETTE, once. Four pictures drawn by four functions in one file will otherwise end up with
        // four backgrounds, and a grid of cloud tiles that do not share a backdrop reads as four different
        // kinds of asset rather than one family.
        constexpr unsigned char kBackdrop[3] = { 24, 28, 34 };

        // ------------------------------------------------------------------------------------------
        // Reading the file
        // ------------------------------------------------------------------------------------------

        // STRAIGHT OFF THE DISK, NOT THROUGH THE VFS, and the difference is deliberate rather than an
        // omission. The asset classes read through Common::Utils::VFS so a PACKAGED game can serve an
        // asset out of a mounted .dpak; this producer only ever runs in the editor, over files the
        // Content Browser is listing from a real directory, and the VFS's mount table is shared mutable
        // state that nothing has declared safe to read from a JobSystem worker. A loose file is what the
        // browser shows and a loose file is what this opens.
        Common::ResultStr<std::vector<unsigned char>> ReadBytes( const std::string& path )
        {
            std::ifstream file( path, std::ios::binary );
            if ( !file )
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "'{}' could not be opened for reading", path );

            std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( file ) ),
                                              std::istreambuf_iterator<char>() );
            if ( bytes.empty() )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "'{}' is empty", path );
            return Common::MakeSuccess( std::move( bytes ) );
        }

        // ------------------------------------------------------------------------------------------
        // Drawing primitives
        // ------------------------------------------------------------------------------------------

        std::vector<unsigned char> Backdrop()
        {
            std::vector<unsigned char> out( kBytes );
            for ( size_t i = 0; i < kBytes; i += 4 )
            {
                out[i + 0] = kBackdrop[0];
                out[i + 1] = kBackdrop[1];
                out[i + 2] = kBackdrop[2];
                out[i + 3] = 255;
            }
            return out;
        }

        /**
         * @brief The average of the source rectangle that output pixel (@p ox, @p oy) covers.
         *
         * ONE RESAMPLER FOR EVERY PRODUCER, and it must handle BOTH directions because the sources do: a
         * `.dclayout` is 512 square at the shipped resolution (an exact copy), a `.dcnv` slice is 128
         * (a 4x upscale), a `.dcmv` projection is 128x64 (an upscale that is not square). A nearest tap
         * would alias the first and a box filter alone would collapse to nearest on the others, so this
         * is the box filter with the degenerate case — a rectangle narrower than one texel — falling out
         * as the single tap it should be.
         *
         * @p channels is the source's stride; @p pick is which of them to read.
         */
        float BoxSample( const unsigned char* src, uint32_t srcW, uint32_t srcH, uint32_t channels, uint32_t pick,
                         uint32_t ox, uint32_t oy, uint32_t outW, uint32_t outH )
        {
            const float x0 = static_cast<float>( ox ) * static_cast<float>( srcW ) / static_cast<float>( outW );
            const float x1 =
                 static_cast<float>( ox + 1 ) * static_cast<float>( srcW ) / static_cast<float>( outW );
            const float y0 = static_cast<float>( oy ) * static_cast<float>( srcH ) / static_cast<float>( outH );
            const float y1 =
                 static_cast<float>( oy + 1 ) * static_cast<float>( srcH ) / static_cast<float>( outH );

            const uint32_t bx0 = std::min( static_cast<uint32_t>( x0 ), srcW - 1u );
            const uint32_t by0 = std::min( static_cast<uint32_t>( y0 ), srcH - 1u );
            const uint32_t bx1 = std::max( bx0 + 1u, std::min( static_cast<uint32_t>( std::ceil( x1 ) ), srcW ) );
            const uint32_t by1 = std::max( by0 + 1u, std::min( static_cast<uint32_t>( std::ceil( y1 ) ), srcH ) );

            float sum   = 0.0f;
            float count = 0.0f;
            for ( uint32_t y = by0; y < by1; ++y )
            {
                for ( uint32_t x = bx0; x < bx1; ++x )
                {
                    sum += static_cast<float>( src[( static_cast<size_t>( y ) * srcW + x ) * channels + pick] );
                    count += 1.0f;
                }
            }
            return count > 0.0f ? sum / count : 0.0f;
        }

        void PutPixel( std::vector<unsigned char>& out, uint32_t x, uint32_t y, float r, float g, float b )
        {
            const size_t at = ( static_cast<size_t>( y ) * kSide + x ) * 4u;
            out[at + 0]     = static_cast<unsigned char>( std::clamp( r, 0.0f, 255.0f ) );
            out[at + 1]     = static_cast<unsigned char>( std::clamp( g, 0.0f, 255.0f ) );
            out[at + 2]     = static_cast<unsigned char>( std::clamp( b, 0.0f, 255.0f ) );
            out[at + 3]     = 255;
        }

        // ------------------------------------------------------------------------------------------
        // `.dclayout` — the painting, as it was painted
        // ------------------------------------------------------------------------------------------

        Common::ResultStr<std::vector<unsigned char>> PaintLayout( const std::vector<unsigned char>& bytes )
        {
            auto decoded = Assets::DecodeCloudLayout( bytes );
            if ( !decoded )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "{}", decoded.GetError() );

            const Assets::CloudLayoutData layout = decoded.ExtractValue();
            const uint32_t                res    = layout.Resolution;
            if ( layout.Pattern.empty() || res == 0u )
                return Common::MakeError<std::vector<unsigned char>>(
                     "the layout carries no pattern, so there is nothing to draw. A layout with only a mask "
                     "is legal on disk, and it has no picture of its own: the mask is a modifier of a "
                     "pattern, not a placement field" );

            // FOUR QUADRANTS, ONE PER SPECIES SLOT, because a layout's four channels are four independent
            // placement fields and folding them into one RGB would make a slot that is empty
            // indistinguishable from a slot that is dark. The quadrants are TL=0, TR=1, BL=2, BR=3, which
            // is the order the layout panel lists them in.
            //
            // Each is tinted rather than grey: at 64 px an artist reads WHICH slot is busy from the hue
            // long before they can read the shape.
            constexpr float kTint[4][3] = {
                 { 1.00f, 0.86f, 0.62f }, // slot 0 — warm
                 { 0.62f, 0.90f, 1.00f }, // slot 1 — cold
                 { 0.78f, 1.00f, 0.72f }, // slot 2 — green
                 { 1.00f, 0.72f, 0.86f }, // slot 3 — magenta
            };

            std::vector<unsigned char> out  = Backdrop();
            const uint32_t             half = kSide / 2u;

            // A PEDESTAL, so the mask has somewhere to go in BOTH directions. The mask is signed about
            // 128 — above it adds cloud, below it removes — and a picture drawn from zero can only show
            // the adding half, because the removing half clamps at black and disappears.
            constexpr float kPedestal = 40.0f;

            float lowest  = 255.0f;
            float highest = 0.0f;

            for ( uint32_t slot = 0; slot < Assets::kCloudLayoutChannels; ++slot )
            {
                const uint32_t offsetX = ( slot % 2u ) * half;
                const uint32_t offsetY = ( slot / 2u ) * half;

                for ( uint32_t y = 0; y < half; ++y )
                {
                    for ( uint32_t x = 0; x < half; ++x )
                    {
                        float value = BoxSample( layout.Pattern.data(), res, res, 4u, slot, x, y, half, half );

                        // THE MASK DIMS THE PATTERN RATHER THAN BECOMING THE ALPHA. 128 is neutral, above
                        // it adds cloud and below it removes (CloudLayoutData::Mask); baking that into the
                        // luminance is what makes the tile show the placement field the BAKE will actually
                        // see. Writing it as alpha instead would hand the compositing decision to whatever
                        // colour the grid happens to sit on, and the same file would read differently in
                        // two panels.
                        if ( !layout.Mask.empty() )
                        {
                            const float mask = BoxSample( layout.Mask.data(), res, res, 1u, 0u, x, y, half, half );
                            value *= std::clamp( mask / 128.0f, 0.0f, 2.0f );

                            // AND THE MASK IS ADDED ON TOP OF ITS OWN MULTIPLICATION, which looks like two
                            // effects of one field and is one effect plus one honesty. The multiplication
                            // is what the BAKE does. The addition is what stops the mask vanishing from
                            // the picture: two shipped layouts (O4_MaskNeutral, O4_MaskAddRemove) carry a
                            // pattern of all zeros and put everything they say into the mask, so a picture
                            // that only multiplied showed a black square for a file with content in it —
                            // the "middle link drops a property" shape, with an authored field as the
                            // property.
                            value += ( mask - 128.0f ) * 0.35f;
                        }

                        value += kPedestal;
                        lowest  = std::min( lowest, value );
                        highest = std::max( highest, value );

                        PutPixel( out, offsetX + x, offsetY + y, value * kTint[slot][0], value * kTint[slot][1],
                                  value * kTint[slot][2] );
                    }
                }
            }

            // A LAYOUT THAT SAYS NOTHING HAS NO PICTURE, and it must SAY so rather than hand back a flat
            // square. A uniform tile is indistinguishable from a producer that failed, and — worse — the
            // freshness rule would then call that flat square a good picture of the asset for ever. The
            // browser's answer for a refused file is the cloud type icon, which is a true statement about
            // a layout that places nothing and modifies nothing.
            if ( highest - lowest < 1.0f )
            {
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "this layout is uniform: all four pattern channels and the mask hold one value "
                     "each ({:.1f} everywhere), so its picture would be a single flat colour — which "
                     "cannot be told apart from a thumbnail that failed to render",
                     lowest );
            }

            // A one-pixel cross between the quadrants, so four dark slots still read as four slots rather
            // than as one empty square.
            for ( uint32_t i = 0; i < kSide; ++i )
            {
                PutPixel( out, i, half, 70.0f, 76.0f, 86.0f );
                PutPixel( out, half, i, 70.0f, 76.0f, 86.0f );
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // ------------------------------------------------------------------------------------------
        // `.dcnv` — the cell structure the cloud edge is cut from
        // ------------------------------------------------------------------------------------------

        Common::ResultStr<std::vector<unsigned char>> PaintNoiseVolume( const std::vector<unsigned char>& bytes )
        {
            auto decoded = Assets::DecodeCloudNoiseVolume( bytes );
            if ( !decoded )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "{}", decoded.GetError() );

            const Assets::CloudNoiseVolumeData volume = decoded.ExtractValue();
            const uint32_t                     n      = volume.Params.Resolution;
            if ( n == 0u || volume.Voxels.size() < static_cast<size_t>( n ) * n * n * 4u )
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "the volume decoded but carries {} payload bytes for a resolution of {} (it needs {})",
                     volume.Voxels.size(), n, static_cast<size_t>( n ) * n * n * 4u );

            // THE MIDDLE SLICE, and the choice matters: slice 0 is a face of a tiling volume, so it is the
            // one plane where a periodic worley lattice lines its cell walls up with the border and looks
            // like a grid. Half way in shows the interior, which is what the erosion actually samples.
            const uint32_t             z     = n / 2u;
            const unsigned char* const plane = volume.Voxels.data() + static_cast<size_t>( z ) * n * n * 4u;

            std::vector<unsigned char> out = Backdrop();
            for ( uint32_t y = 0; y < kSide; ++y )
            {
                for ( uint32_t x = 0; x < kSide; ++x )
                {
                    // R, G and the two fine channels averaged — the same fold the noise panel's "all four"
                    // view uses, so the tile and the document that opens from it show one picture.
                    const float r = BoxSample( plane, n, n, 4u, 0u, x, y, kSide, kSide );
                    const float g = BoxSample( plane, n, n, 4u, 1u, x, y, kSide, kSide );
                    const float b = 0.5f * ( BoxSample( plane, n, n, 4u, 2u, x, y, kSide, kSide ) +
                                             BoxSample( plane, n, n, 4u, 3u, x, y, kSide, kSide ) );
                    PutPixel( out, x, y, r, g, b );
                }
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // ------------------------------------------------------------------------------------------
        // `.dcmv` — the sculpted body, side on
        // ------------------------------------------------------------------------------------------

        Common::ResultStr<std::vector<unsigned char>>
        PaintModellingVolume( const std::vector<unsigned char>& bytes )
        {
            auto decoded = Assets::DecodeCloudModellingVolume( bytes );
            if ( !decoded )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "{}", decoded.GetError() );

            const Assets::CloudModellingVolumeData body = decoded.ExtractValue();
            if ( body.Voxels.size() < Assets::kCloudModellingVoxelBytes )
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "the volume decoded but carries {} payload bytes where the format fixes {}",
                     body.Voxels.size(), Assets::kCloudModellingVoxelBytes );

            constexpr uint32_t w = Assets::kCloudModellingVolumeWidth;  // x
            constexpr uint32_t h = Assets::kCloudModellingVolumeHeight; // y, up
            constexpr uint32_t d = Assets::kCloudModellingVolumeDepth;  // z

            // MAXIMUM ALONG THE VIEW AXIS, NOT A SLICE. A hero body is a handful of lumps in a mostly
            // empty box, so a slice through the middle of it is very often empty — the artist would get a
            // black tile for a cloud they had just sculpted. The maximum over z is the SILHOUETTE, which
            // is the thing a body is recognised by and the thing that cannot be empty while the body is
            // not.
            //
            // NO BAKE HAPPENS HERE. The 4 MiB of voxels are in the file and `DecodeCloudModellingVolume`
            // hands them over; the only slicer the engine already had (`GenerateCloudModellingSlice`)
            // re-evaluates the RECIPE instead, which would have made this tile cost a fraction of a bake
            // per asset for a picture the file already contains.
            std::vector<unsigned char> projection( static_cast<size_t>( w ) * h, 0u );
            for ( uint32_t y = 0; y < h; ++y )
            {
                for ( uint32_t x = 0; x < w; ++x )
                {
                    unsigned char best = 0u;
                    for ( uint32_t z = 0; z < d; ++z )
                    {
                        // Channel 0 is the profile/density the march reads; the other three are the
                        // material terms and say nothing about the shape.
                        const size_t at = ( ( static_cast<size_t>( z ) * h + y ) * w + x ) *
                                          Assets::kCloudModellingBytesPerVoxel;
                        best = std::max( best, body.Voxels[at] );
                    }
                    // The volume's y runs UP and an image's rows run DOWN, so the row is flipped here
                    // rather than at the sampler: a hero cloud drawn upside down is a picture that looks
                    // plausible and is wrong, which is the worst kind.
                    projection[static_cast<size_t>( h - 1u - y ) * w + x] = best;
                }
            }

            // 128 x 64 into a square: full width, letterboxed vertically. Stretching to fill would make
            // every body twice as tall as it is, and the proportions are the point of a side view.
            std::vector<unsigned char> out     = Backdrop();
            const uint32_t             bandH   = kSide / 2u; // h/w == 1/2 exactly, by the format's constants
            const uint32_t             bandTop = ( kSide - bandH ) / 2u;

            for ( uint32_t y = 0; y < bandH; ++y )
            {
                for ( uint32_t x = 0; x < kSide; ++x )
                {
                    const float v = BoxSample( projection.data(), w, h, 1u, 0u, x, y, kSide, bandH );
                    // Warm white cloud over the backdrop, so an empty column stays the backdrop rather
                    // than becoming black — a black tile and a missing tile look the same.
                    const float t = std::clamp( v / 255.0f, 0.0f, 1.0f );
                    PutPixel( out, x, bandTop + y, kBackdrop[0] + t * ( 240.0f - kBackdrop[0] ),
                              kBackdrop[1] + t * ( 243.0f - kBackdrop[1] ),
                              kBackdrop[2] + t * ( 250.0f - kBackdrop[2] ) );
                }
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // ------------------------------------------------------------------------------------------
        // `.decloudtype` — the species' own silhouette
        // ------------------------------------------------------------------------------------------

        Common::ResultStr<std::vector<unsigned char>> PaintCloudType( const std::vector<unsigned char>& bytes )
        {
            auto parsed = Assets::ParseCloudType( std::string( bytes.begin(), bytes.end() ) );
            if ( !parsed )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "{}", parsed.GetError() );

            const Graphic::CloudTypeShape shape = parsed.ExtractValue().Shape;

            // THE PICTURE IS THE PROFILE, FILLED. Half-width against height up the type's own band,
            // mirrored about the centre — the silhouette the lump stack builds, which is what makes a
            // stratus a deck and a congestus a tower.
            //
            // TWO OUTLINES, because a type has two: the CORE of a placement patch reaches TopAltitudeKm,
            // and the edge of the patch reaches only `EdgeTopFraction` of the band. The panel plots the
            // same pair as two curves ("two lines through one cloud"); here the taller one is drawn
            // bright over the shorter one drawn dim, which is the same fact as a picture.
            //
            // THE BAND FILLS THE SQUARE, so absolute altitude is NOT encoded. That is a deliberate loss:
            // the shipped library spans 0.4 km (stratus) to 12 km (cirrus), so a common altitude axis
            // would draw eight of the nine types as a sliver three pixels tall. Height is in the file, in
            // the name and in the Details row; the shape is only here.
            //
            // THE ANVIL IS NOT DRAWN, and that is not an omission: an anvil is a SECOND LOBE OF THE
            // PLACEMENT (AnvilAltitudeKm, AnvilStrength), added when the volume is built, not a part of
            // the vertical profile this picture is of. Drawing it here would make the tile assert
            // something the profile does not say.
            float maxHalfWidth = 0.0f;
            for ( const float w : shape.Profile.HalfWidth )
                maxHalfWidth = std::max( maxHalfWidth, w );
            if ( !( maxHalfWidth > 0.0f ) )
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "every one of the {} profile samples is zero or negative, so this type has no "
                     "silhouette to draw",
                     static_cast<int>( Graphic::kCloudProfileSamples ) );

            std::vector<unsigned char> out = Backdrop();

            // Margins: the silhouette occupies the middle 84% of the square, so a wide type does not sit
            // flush against the tile border where the grid's own frame would eat its edge.
            constexpr float kMargin    = 0.08f;
            const float     usableHalf = ( 0.5f - kMargin ) * static_cast<float>( kSide );
            const float     baseY      = ( 1.0f - kMargin ) * static_cast<float>( kSide );
            const float     bandPixels = ( 1.0f - 2.0f * kMargin ) * static_cast<float>( kSide );
            const float     centreX    = 0.5f * static_cast<float>( kSide );

            const float edgeFraction = std::clamp( shape.EdgeTopFraction, 0.0f, 1.0f );

            // THE SHADING IS NOT DECORATION, and the first version of this function proved it: two flat
            // fills produced a square with TWO colours in it for five of the nine shipped types, because
            // the taller CORE silhouette covers the shorter EDGE one at every height — the inner shape
            // was drawn and then painted over completely. A flat silhouette is also, at 64 px, a black
            // blob: the very thing this whole task replaces.
            //
            // So each layer is shaded by two quantities the profile already carries: HEIGHT up its own
            // band (a cloud is lit from above) and DISTANCE from the axis (a round body falls off towards
            // its flanks). The dim layer is the core's full extent and the bright one is the flank's
            // shorter body drawn inside it, so both are visible and the picture reads as a lit shape
            // rather than as a chart. Desert/Tests/Editor/ThumbnailFormats asserts the square is not a
            // flat fill, which is the assertion that caught the first version.
            struct Layer
            {
                float TopFraction;
                float Floor; ///< brightness at the base of this layer's flank
                float Range; ///< how much brighter its lit top and centre get
            };
            const Layer layers[2] = { { 1.0f, 70.0f, 105.0f }, { edgeFraction, 120.0f, 130.0f } };

            for ( const Layer& layer : layers )
            {
                if ( !( layer.TopFraction > 0.0f ) )
                    continue;

                for ( uint32_t y = 0; y < kSide; ++y )
                {
                    // Height fraction of THIS layer's band, from its base (bottom) to its own top.
                    const float up = ( baseY - static_cast<float>( y ) ) / ( bandPixels * layer.TopFraction );
                    if ( up < 0.0f || up > 1.0f )
                        continue;

                    const float halfWidth =
                         Graphic::CloudProfileHalfWidth( shape.Profile, up ) / maxHalfWidth * usableHalf;
                    if ( !( halfWidth > 0.0f ) )
                        continue;

                    const uint32_t from = static_cast<uint32_t>( std::max( 0.0f, centreX - halfWidth ) );
                    const uint32_t to =
                         static_cast<uint32_t>( std::min( static_cast<float>( kSide ), centreX + halfWidth ) );
                    for ( uint32_t x = from; x < to; ++x )
                    {
                        const float across =
                             std::clamp( ( static_cast<float>( x ) - centreX ) / halfWidth, -1.0f, 1.0f );
                        const float body  = 1.0f - across * across; // 1 on the axis, 0 at the flank
                        const float lit   = 0.45f * up + 0.55f * body;
                        const float value = layer.Floor + layer.Range * lit;

                        // Slightly cooler in the shadowed lower flanks, as a cloud is against the sky.
                        PutPixel( out, x, y, value, value * 1.01f, value * 1.06f );
                    }
                }
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // ------------------------------------------------------------------------------------------
        // `.detheme` — the palette, which is what a theme IS
        // ------------------------------------------------------------------------------------------
        //
        // NOT A CLOUD, AND THIS FILE'S NAME NO LONGER DESCRIBES IT. What every producer here has in
        // common is the property the file's own header argues for — the picture is COMPUTED ON THE CPU
        // from the asset's own bytes, with no device and no renderer slot — and a theme has exactly that
        // property for a better reason than any cloud format: a theme's picture is its colours, and its
        // colours are literally the file's contents. Renaming this translation unit to match (it is
        // `CpuThumbnail`, not `CloudThumbnail`) is a rename across ThumbnailService and two suites, and
        // is named as debt rather than done here.
        //
        // WHY A STRIP AND NOT A SWATCH GRID. A theme has tens of tokens and a 64-pixel tile has room for
        // about eight distinguishable bands, so a grid of every token would be a mosaic nobody can read.
        // The strip takes the palette IN FILE ORDER, which is the order a theme author writes it in and
        // therefore the order in which the load-bearing colours come first — a surface, then an accent.
        Common::ResultStr<std::vector<unsigned char>> PaintUITheme( const std::vector<unsigned char>& bytes )
        {
            auto parsed = Assets::ParseUITheme( std::string( bytes.begin(), bytes.end() ) );
            if ( !parsed )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "{}", parsed.GetError() );

            const Assets::UIThemeData theme = parsed.ExtractValue();
            if ( theme.Colors.empty() )
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "the theme declares no colours at all, so its picture would be an empty square — "
                     "indistinguishable from a producer that failed, which is the one thing a thumbnail "
                     "must never be" );

            std::vector<unsigned char> out = Backdrop();

            // A margin, so the tile reads as a swatch card rather than as a full-bleed colour — the
            // backdrop is what makes a grid of themes look like one family of assets.
            constexpr uint32_t kMargin = kSide / 10u;
            const uint32_t     top     = kMargin;
            const uint32_t     bottom  = kSide - kMargin;
            const uint32_t     left    = kMargin;
            const uint32_t     right   = kSide - kMargin;

            const std::size_t bands = std::min<std::size_t>( theme.Colors.size(), 8u );
            const float       bandH = static_cast<float>( bottom - top ) / static_cast<float>( bands );

            for ( std::size_t b = 0; b < bands; ++b )
            {
                const glm::vec3 c = theme.Colors[b].Value;
                // The palette is LINEAR, as every colour field in this engine is, and a thumbnail is
                // shown in sRGB. Without the encode a dark surface and a slightly darker one are two
                // indistinguishable near-blacks — the exact failure the flat-square assertion exists for.
                const auto Encode = []( float v )
                {
                    const float clamped = std::clamp( v, 0.0f, 1.0f );
                    return 255.0f * std::pow( clamped, 1.0f / 2.2f );
                };

                const uint32_t y0 = top + static_cast<uint32_t>( static_cast<float>( b ) * bandH );
                const uint32_t y1 = top + static_cast<uint32_t>( static_cast<float>( b + 1 ) * bandH );
                for ( uint32_t y = y0; y < y1 && y < bottom; ++y )
                    for ( uint32_t x = left; x < right; ++x )
                        PutPixel( out, x, y, Encode( c.r ), Encode( c.g ), Encode( c.b ) );
            }

            return Common::MakeSuccess( std::move( out ) );
        }
    } // namespace

    Common::ResultStr<std::vector<unsigned char>> Paint( const std::string& assetPath )
    {
        // THROUGH THE CENSUS, not through a private chain of `if`s. The set of formats this function can
        // paint and the set the table calls `Producer::Painted` are the same set by construction, so a
        // fifth cloud format added to one and forgotten in the other cannot exist: the row would arrive
        // here and be refused by name, and Desert/Tests/Editor/ThumbnailFormats paints every Painted row
        // and would go red on it.
        const std::string ext = ThumbnailFormats::ExtensionOf( assetPath );

        auto bytes = ReadBytes( assetPath );
        if ( !bytes )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "{}", bytes.GetError() );

        const std::vector<unsigned char> payload = bytes.ExtractValue();

        if ( ext == "dclayout" )
            return PaintLayout( payload );
        if ( ext == "dcnv" )
            return PaintNoiseVolume( payload );
        if ( ext == "dcmv" )
            return PaintModellingVolume( payload );
        if ( ext == "decloudtype" )
            return PaintCloudType( payload );
        if ( ext == "detheme" )
            return PaintUITheme( payload );

        return Common::MakeFormattedError<std::vector<unsigned char>>(
             "'{}' has extension '{}', which no CPU thumbnail producer claims. If the Content Browser "
             "shows this format it needs a row in Editor/Widgets/ThumbnailFormats.hpp, and a "
             "Producer::Painted row needs a branch here.",
             assetPath, ext );
    }

    Common::BoolResultStr Write( const std::string& assetPath, const std::string& png )
    {
        auto painted = Paint( assetPath );
        if ( !painted )
            return Common::MakeFormattedError<bool>( "{}", painted.GetError() );

        const std::vector<unsigned char> pixels = painted.ExtractValue();

        std::error_code ec;
        std::filesystem::create_directories( std::filesystem::path( png ).parent_path(), ec );

        stbi_flip_vertically_on_write( 0 ); // the buffer's first row is the top row

        // WRITE ASIDE, THEN RENAME — the same discipline AssetThumbnailRenderer keeps, and for the same
        // reason: stbi_write_png streams into the destination, so a process that dies mid-write leaves a
        // truncated PNG at the cache path. That file is FRESH by modification time and undecodable
        // for ever, which is the one state the freshness rule cannot repair.
        const std::string temp = png + ".part";
        if ( !stbi_write_png( temp.c_str(), static_cast<int>( kSide ), static_cast<int>( kSide ), 4, pixels.data(),
                              static_cast<int>( kSide ) * 4 ) )
        {
            return Common::MakeFormattedError<bool>( "stbi_write_png refused to write '{}' ({}x{} RGBA8)", temp,
                                                     kSide, kSide );
        }

        std::filesystem::rename( temp, png, ec );
        if ( ec )
        {
            std::error_code cleanupEc;
            std::filesystem::remove( temp, cleanupEc );
            return Common::MakeFormattedError<bool>(
                 "'{}' was painted but could not be moved into place from '{}': {}", png, temp, ec.message() );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::CloudThumbnail
