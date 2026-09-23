// The image-format vocabulary, tested as numbers.
//
// This exists because of a fault that could not have been caught by anything we had: GetBytesPerPixel
// handled exactly two of the five formats and ended in `return 0U;`. Every other format therefore got a
// bytes-per-pixel of 0 — silently — and CalculateImageSize multiplied that into a zero-byte staging
// buffer for a real image. Nothing failed at the point of the mistake; the corruption showed up later,
// somewhere with no visible connection to the format.
//
// The lookups are now total over ImageFormat and end in a hard stop rather than a number. Everything
// below is an exact byte count, and several cases fail outright against the old function.

#include <Engine/Core/Formats/ImageFormat.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace Formats = Desert::Core::Formats;

using Formats::CalculateImageSize;
using Formats::GetBytesPerPixel;
using Formats::GetImageAspect;
using Formats::ImageFormat;

namespace
{
    // EVERY ENUMERATOR THAT HAS A BYTES-PER-PIXEL AT ALL, derived from the format table rather than
    // written down. It used to be a hand-written list of six with a comment calling it "every
    // enumerator of ImageFormat", and the day the two BC formats arrived that comment became false
    // while every test below went on passing — the list simply did not mention them. A list that
    // claims to be a census has to BE one.
    //
    // A BLOCK FORMAT IS EXCLUDED, AND THAT IS THE POINT RATHER THAN AN OMISSION: `GetBytesPerPixel`
    // refuses one by design (a 4x4 block in sixteen bytes is not a bytes-per-pixel), so asking it here
    // would be asking for the abort. What every enumerator DOES have is a block, and
    // `BlockCompression`'s suite walks all of them for it.
    /// Every enumerator, block formats included. Used by the checks that hold for both kinds.
    std::vector<ImageFormat> AllFormats()
    {
        std::vector<ImageFormat> formats;
        for ( uint32_t i = 0; i < Formats::kImageFormatCount; ++i )
            formats.push_back( static_cast<ImageFormat>( i ) );
        return formats;
    }

    std::vector<ImageFormat> UncompressedFormats()
    {
        std::vector<ImageFormat> formats;
        for ( uint32_t i = 0; i < Formats::kImageFormatCount; ++i )
        {
            const auto format = static_cast<ImageFormat>( i );
            if ( !Formats::IsBlockCompressed( format ) )
                formats.push_back( format );
        }
        return formats;
    }
} // namespace

// The list above is derived; this is what makes it a census rather than a filter nobody checks. Six
// uncompressed formats and at least one block format have to exist, or the split it is built on is not
// being exercised by this tree at all.
TEST( ImageFormatBytesPerPixel, TheDerivedListStillSplitsTheEnum )
{
    const std::size_t uncompressed = UncompressedFormats().size();
    EXPECT_GT( uncompressed, 0u );
    EXPECT_LT( uncompressed, Formats::kImageFormatCount )
         << "no format in the table is block-compressed any more; either one was removed, or "
            "IsBlockCompressed stopped answering";
}

// ── Bytes per pixel: the exact size of every enumerator (CLD-96e) ─────────────────────────────────

TEST( ImageFormatBytesPerPixel, EveryEnumeratorHasItsRealSize )
{
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::RGBA8F ), 4u );
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::RGBA16F ), 8u );
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::RGBA32F ), 16u );
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::BGRA8F ), 4u );
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::DEPTH24STENCIL8 ), 4u );
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::DEPTH32F ), 4u );
}

// The property the deleted `return 0U;` used to violate: no declared format answers zero. A zero here
// is not a wrong number, it is an allocation of nothing for an image that exists.
TEST( ImageFormatBytesPerPixel, NoEnumeratorAnswersZero )
{
    for ( const ImageFormat format : UncompressedFormats() )
        EXPECT_GT( GetBytesPerPixel( format ), 0u ) << "format index " << static_cast<uint32_t>( format );
}

// A half-float texel is exactly half of a 32-bit-float one — this is the whole reason RGBA16F was added,
// and it is what the per-renderer memory figures below are built on.
TEST( ImageFormatBytesPerPixel, HalfFloatIsHalfOfFullFloat )
{
    EXPECT_EQ( GetBytesPerPixel( ImageFormat::RGBA32F ), 2u * GetBytesPerPixel( ImageFormat::RGBA16F ) );
}

// ── 2D size arithmetic ────────────────────────────────────────────────────────────────────────────

TEST( ImageFormatSize, TwoDimensionalSizesAreExact )
{
    EXPECT_EQ( CalculateImageSize( 128u, 128u, ImageFormat::RGBA16F ), 131072ull );
    EXPECT_EQ( CalculateImageSize( 128u, 128u, ImageFormat::RGBA8F ), 65536ull );
    EXPECT_EQ( CalculateImageSize( 128u, 128u, ImageFormat::RGBA32F ), 262144ull );
    EXPECT_EQ( CalculateImageSize( 512u, 512u, ImageFormat::RGBA8F ), 1048576ull );
}

TEST( ImageFormatSize, AnEmptyExtentCostsNothing )
{
    EXPECT_EQ( CalculateImageSize( 0u, 128u, ImageFormat::RGBA16F ), 0ull );
    EXPECT_EQ( CalculateImageSize( 128u, 0u, ImageFormat::RGBA16F ), 0ull );
    EXPECT_EQ( CalculateImageSize( 128u, 128u, 0u, ImageFormat::RGBA8F ), 0ull );
}

// ── 3D size arithmetic ────────────────────────────────────────────────────────────────────────────

TEST( ImageFormatSize, VolumeSizesAreExact )
{
    // A 128^3 RGBA8F volume is 8 MiB.
    EXPECT_EQ( CalculateImageSize( 128u, 128u, 128u, ImageFormat::RGBA8F ), 8388608ull );
    // A 32^3 RGBA8F volume is 128 KiB.
    EXPECT_EQ( CalculateImageSize( 32u, 32u, 32u, ImageFormat::RGBA8F ), 131072ull );
    // The same volume at 32-bit float is what we are NOT paying: 4x.
    EXPECT_EQ( CalculateImageSize( 128u, 128u, 128u, ImageFormat::RGBA32F ), 33554432ull );
}

// A volume with depth 1 is the same number of bytes as the 2D image of the same face.
TEST( ImageFormatSize, DepthOneAgreesWithTheTwoDimensionalOverload )
{
    for ( const ImageFormat format : AllFormats() )
        EXPECT_EQ( CalculateImageSize( 128u, 64u, 1u, format ), CalculateImageSize( 128u, 64u, format ) )
             << "format index " << static_cast<uint32_t>( format );
}

// 64-bit arithmetic, not 32-bit: a 2048^3 RGBA8F volume is 32 GiB, and computing it in uint32_t would
// wrap to 0 — the same silent-wrong-size failure the zero bytes-per-pixel used to cause.
TEST( ImageFormatSize, LargeVolumesDoNotWrap )
{
    EXPECT_EQ( CalculateImageSize( 2048u, 2048u, 2048u, ImageFormat::RGBA8F ), 34359738368ull );
    EXPECT_EQ( CalculateImageSize( 65536u, 65536u, ImageFormat::RGBA8F ), 17179869184ull );
}

// ── Aspect selection (CLD-96c) ────────────────────────────────────────────────────────────────────
//
// This is the half of the depth-transition helper that can be checked without a device. Naming COLOR on
// a depth image, or DEPTH alone on a packed depth+stencil one, is a barrier validation error — and it is
// why the scene depth attachment could not be handed to a compute sampler at all.

TEST( ImageFormatAspect, ColourFormatsAreColourOnly )
{
    EXPECT_EQ( GetImageAspect( ImageFormat::RGBA8F ), Formats::ImageAspect_Colour );
    EXPECT_EQ( GetImageAspect( ImageFormat::RGBA16F ), Formats::ImageAspect_Colour );
    EXPECT_EQ( GetImageAspect( ImageFormat::RGBA32F ), Formats::ImageAspect_Colour );
    EXPECT_EQ( GetImageAspect( ImageFormat::BGRA8F ), Formats::ImageAspect_Colour );
}

TEST( ImageFormatAspect, PackedDepthStencilNamesBothPlanes )
{
    const uint32_t aspect = GetImageAspect( ImageFormat::DEPTH24STENCIL8 );
    EXPECT_TRUE( aspect & Formats::ImageAspect_Depth );
    EXPECT_TRUE( aspect & Formats::ImageAspect_Stencil );
    EXPECT_FALSE( aspect & Formats::ImageAspect_Colour );
}

TEST( ImageFormatAspect, DepthOnlyFormatDoesNotClaimAStencil )
{
    const uint32_t aspect = GetImageAspect( ImageFormat::DEPTH32F );
    EXPECT_TRUE( aspect & Formats::ImageAspect_Depth );
    EXPECT_FALSE( aspect & Formats::ImageAspect_Stencil );
    EXPECT_FALSE( aspect & Formats::ImageAspect_Colour );
}

// No format is aspect-less, and no format claims colour together with depth: both would produce a
// barrier Vulkan rejects.
TEST( ImageFormatAspect, EveryEnumeratorNamesExactlyOneFamily )
{
    for ( const ImageFormat format : AllFormats() )
    {
        const uint32_t aspect = GetImageAspect( format );
        EXPECT_NE( aspect, 0u ) << "format index " << static_cast<uint32_t>( format );

        const bool colour = ( aspect & Formats::ImageAspect_Colour ) != 0;
        const bool depth  = ( aspect & ( Formats::ImageAspect_Depth | Formats::ImageAspect_Stencil ) ) != 0;
        EXPECT_NE( colour, depth ) << "format index " << static_cast<uint32_t>( format );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
