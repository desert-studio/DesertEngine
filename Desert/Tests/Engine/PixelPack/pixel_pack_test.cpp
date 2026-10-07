// A CAPTURED PIXEL MUST SAY WHAT THE DEVICE SAID.
//
// This engine now has two readbacks, and they answer different questions. The old one reads the scene's own
// offscreen image; the new one reads the PRESENTED swapchain frame, which is the only one of the two that
// contains the interface at all -- ImGui is recorded into the swapchain render pass, so until that readback
// existed no capture this engine ever took held a single pixel of a panel, a menu or a dialog.
//
// The two arrive at their bytes by different routes: one from the engine's ImageFormat, one from whatever
// VkFormat the surface negotiated. Written twice, the pack would eventually disagree with itself, and the
// symptom is not a crash -- it is a capture whose red and blue are exchanged, which reads as a rendering
// defect and gets reported as one. So the MAPPINGS stay at their call sites, where the format vocabularies
// are, and the ARITHMETIC lives in one header with no Vulkan in it.
//
// The relations:
//   1. RGBA passes through unchanged, byte for byte. A capture must not be quietly re-coloured.
//   2. BGRA exchanges red and blue, and ONLY red and blue.
//   3. Floats are clamped and quantised, and the endpoints land exactly on 0 and 255.
//   4. A SHORT BUFFER PRODUCES NOTHING. A failed readback that got packed anyway would be a plausible
//      image of uninitialised memory -- a picture that looks exactly like evidence.
//   5. The size the pack expects and the size the caller allocates come from ONE function, so a staging
//      buffer cannot be allocated to a different size from the one that is read.

#include <Engine/Graphic/PixelPack.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using Desert::Graphic::BytesPerPixel;
using Desert::Graphic::PackedPixelSource;
using Desert::Graphic::PackToRGBA8;

namespace
{
    std::vector<uint8_t> FloatBytes( const std::vector<float>& values )
    {
        std::vector<uint8_t> bytes( values.size() * sizeof( float ) );
        std::memcpy( bytes.data(), values.data(), bytes.size() );
        return bytes;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1-2. Channel order.
// ---------------------------------------------------------------------------------------------------

TEST( PixelPack, RgbaPassesThroughUnchanged )
{
    const std::vector<uint8_t> raw = { 10, 20, 30, 40, 200, 150, 100, 50 };

    const std::vector<uint8_t> packed = PackToRGBA8( raw.data(), raw.size(), 2, PackedPixelSource::RGBA8 );
    EXPECT_EQ( packed, raw ) << "a capture must not be re-coloured on its way to the file";
}

// The surface this engine actually negotiates on macOS is BGRA, so this is the path every window capture
// takes. Red and blue are exchanged; green and ALPHA are not -- swapping four channels instead of two is
// the easy version of this mistake and produces an image that looks almost right.
TEST( PixelPack, BgraExchangesRedAndBlueAndNothingElse )
{
    const std::vector<uint8_t> raw = { 10, 20, 30, 40 }; // B=10, G=20, R=30, A=40

    const std::vector<uint8_t> packed = PackToRGBA8( raw.data(), raw.size(), 1, PackedPixelSource::BGRA8 );
    ASSERT_EQ( packed.size(), 4u );
    EXPECT_EQ( packed[0], 30 ) << "red must come from the third source byte";
    EXPECT_EQ( packed[1], 20 ) << "green is untouched";
    EXPECT_EQ( packed[2], 10 ) << "blue must come from the first source byte";
    EXPECT_EQ( packed[3], 40 ) << "alpha is untouched";
}

// The swizzle is its own inverse: packing BGRA twice returns the original. A one-way transform that got the
// direction wrong would pass a test that only checked "something changed".
TEST( PixelPack, TheBgraSwizzleIsItsOwnInverse )
{
    const std::vector<uint8_t> raw = { 1, 2, 3, 4, 250, 251, 252, 253 };

    const std::vector<uint8_t> once  = PackToRGBA8( raw.data(), raw.size(), 2, PackedPixelSource::BGRA8 );
    const std::vector<uint8_t> twice = PackToRGBA8( once.data(), once.size(), 2, PackedPixelSource::BGRA8 );
    EXPECT_EQ( twice, raw );
}

// ---------------------------------------------------------------------------------------------------
// 3. Floats.
// ---------------------------------------------------------------------------------------------------

TEST( PixelPack, FloatsAreClampedAndQuantisedWithExactEndpoints )
{
    const std::vector<uint8_t> raw = FloatBytes( { 0.0f, 1.0f, 0.5f, -3.0f } );

    const std::vector<uint8_t> packed = PackToRGBA8( raw.data(), raw.size(), 1, PackedPixelSource::RGBA32F );
    ASSERT_EQ( packed.size(), 4u );
    EXPECT_EQ( packed[0], 0 ) << "black must be exactly 0, not 1";
    EXPECT_EQ( packed[1], 255 ) << "white must be exactly 255, not 254";
    EXPECT_EQ( packed[2], 128 ) << "0.5 rounds rather than truncating";
    EXPECT_EQ( packed[3], 0 ) << "a negative value clamps rather than wrapping to 253";
}

TEST( PixelPack, FloatsAboveOneClampRatherThanWrapping )
{
    const std::vector<uint8_t> raw = FloatBytes( { 4.0f, 1.0001f, 255.0f, 1e30f } );

    const std::vector<uint8_t> packed = PackToRGBA8( raw.data(), raw.size(), 1, PackedPixelSource::RGBA32F );
    ASSERT_EQ( packed.size(), 4u );
    for ( uint8_t channel : packed )
        EXPECT_EQ( channel, 255 ) << "an over-bright value must saturate, not wrap to something dark";
}

// ---------------------------------------------------------------------------------------------------
// 4. A short buffer produces nothing.
// ---------------------------------------------------------------------------------------------------

// THE ONE THAT MATTERS MOST. A readback that failed halfway leaves a buffer shorter than the image; packed
// anyway, it produces a perfectly plausible picture of whatever memory was there. That is worse than an
// error, because it looks like evidence and gets filed as evidence.
TEST( PixelPack, AShortBufferPacksToNothingRatherThanToGarbage )
{
    const std::vector<uint8_t> raw = { 1, 2, 3, 4, 5, 6 }; // one and a half pixels

    EXPECT_TRUE( PackToRGBA8( raw.data(), raw.size(), 2, PackedPixelSource::RGBA8 ).empty() );
    EXPECT_TRUE( PackToRGBA8( raw.data(), raw.size(), 2, PackedPixelSource::BGRA8 ).empty() );
    EXPECT_TRUE( PackToRGBA8( raw.data(), raw.size(), 1, PackedPixelSource::RGBA32F ).empty() )
         << "one float pixel needs 16 bytes and got 6";
}

TEST( PixelPack, ANullBufferPacksToNothing )
{
    EXPECT_TRUE( PackToRGBA8( nullptr, 0, 4, PackedPixelSource::RGBA8 ).empty() );
}

TEST( PixelPack, AnExactlySizedBufferIsAccepted )
{
    const std::vector<uint8_t> raw( 4 * 4, 7 );
    EXPECT_EQ( PackToRGBA8( raw.data(), raw.size(), 4, PackedPixelSource::RGBA8 ).size(), 16u );
}

// ---------------------------------------------------------------------------------------------------
// 5. One source of the size.
// ---------------------------------------------------------------------------------------------------

// The staging buffer the caller allocates and the length the pack checks come from this one function. Two
// separate expressions for "how big is a pixel" is how a readback ends up allocating for one layout and
// reading as another -- and on the 8-bit path the sizes agree, so it would work until the first float
// target and then produce a quarter of an image.
TEST( PixelPack, TheSourceSizeIsAskedOfOneFunction )
{
    EXPECT_EQ( BytesPerPixel( PackedPixelSource::RGBA8 ), 4u );
    EXPECT_EQ( BytesPerPixel( PackedPixelSource::BGRA8 ), 4u );
    EXPECT_EQ( BytesPerPixel( PackedPixelSource::RGBA32F ), 16u );

    // And the pack agrees with it: a buffer of exactly BytesPerPixel * pixels is enough, one byte less is
    // not. That is the relation, rather than the three numbers above on their own.
    for ( const auto source : { PackedPixelSource::RGBA8, PackedPixelSource::BGRA8, PackedPixelSource::RGBA32F } )
    {
        const std::size_t          pixels = 3;
        const std::size_t          needed = pixels * BytesPerPixel( source );
        const std::vector<uint8_t> exact( needed, 0 );
        const std::vector<uint8_t> short_( needed - 1, 0 );

        EXPECT_EQ( PackToRGBA8( exact.data(), exact.size(), pixels, source ).size(), pixels * 4u );
        EXPECT_TRUE( PackToRGBA8( short_.data(), short_.size(), pixels, source ).empty() );
    }
}
