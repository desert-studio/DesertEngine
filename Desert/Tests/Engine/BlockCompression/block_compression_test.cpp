// THE BLOCK ENCODER, AND THE ONLY INSTRUMENT ON THIS MACHINE THAT CAN FAIL IT.
//
// WHY THE DEVICE IS NOT THE WITNESS, measured 2026-09-23 and recorded in `TextureCompressionBC`: an 8x8
// `VK_FORMAT_BC7_UNORM_BLOCK` image built from four hand-written mode-6 blocks, uploaded through a
// staging buffer and `texelFetch`ed back, returns all 64 texels bit-exact on MoltenVK 1.1.357 WHETHER OR
// NOT `textureCompressionBC` was enabled, with no validation message either way. A green run against the
// GPU here proves nothing about whether these blocks are right. So the encoder is measured against a
// DECODER written from the specification (`Core/Formats/BlockCompression.cpp`), and the thing that would
// catch a mis-packed field is that the two disagree with the pixels that went in.
//
// FOUR KINDS OF CHECK, IN ORDER OF HOW EARLY THEY CATCH A MISTAKE:
//
//   1. THE ARITHMETIC over every enumerator of the format table: a level narrower than a block is a
//      WHOLE block, and the sum is the pitch times the block rows. It runs for formats that do not
//      exist yet, because it is written against `kImageFormatCount`.
//   2. THE PACKING: the bits this encoder writes are read back by name, so a field that moved by one
//      bit is named rather than merely producing worse pixels.
//   3. THE RECONSTRUCTION: what survives a round trip exactly (a flat block, a two-colour block), and
//      what survives approximately.
//   4. THE CORPUS REGISTER: every source image in this repository, one row each, with the verdict the
//      cook's measured policy reaches on it. That is the test that would notice the encoder getting
//      worse, and it is written as VERDICTS per named file rather than as a number, so it cannot be
//      satisfied by editing a number.

#include <gtest/gtest.h>

#include <Engine/Core/Formats/BlockCompression.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Fmt = Desert::Core::Formats;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    /// Every block format the table knows, derived from the table rather than listed here — so a third
    /// block format added tomorrow is exercised by all of the arithmetic tests without anyone editing
    /// this file, and a format added WITHOUT a block breaks the build long before it reaches here.
    std::vector<Fmt::ImageFormat> BlockFormats()
    {
        std::vector<Fmt::ImageFormat> formats;
        for ( uint32_t i = 0; i < Fmt::kImageFormatCount; ++i )
        {
            const auto format = static_cast<Fmt::ImageFormat>( i );
            if ( Fmt::IsBlockCompressed( format ) )
                formats.push_back( format );
        }
        return formats;
    }
} // namespace

// ── 1. THE ARITHMETIC ─────────────────────────────────────────────────────────────────────────────

TEST( BlockCompression, EveryFormatSizesAnImageAsWholeBlocks )
{
    // OVER THE WHOLE ENUM, uncompressed formats included: the identity being asserted is the one that
    // makes a single `CalculateImageSize` correct for both kinds, so both kinds have to satisfy it.
    for ( uint32_t i = 0; i < Fmt::kImageFormatCount; ++i )
    {
        const auto format = static_cast<Fmt::ImageFormat>( i );
        const auto block  = Fmt::GetTexelBlock( format );
        ASSERT_GT( block.Width, 0u );
        ASSERT_GT( block.Height, 0u );
        ASSERT_GT( block.Bytes, 0u );

        for ( uint32_t width = 1; width <= 17; ++width )
        {
            for ( uint32_t height = 1; height <= 17; ++height )
            {
                const uint32_t across = Fmt::BlocksAcross( width, format );
                const uint32_t down   = Fmt::BlocksDown( height, format );

                // ROUNDED UP, NEVER DOWN. A level of six texels in a four-texel block is two blocks;
                // one and a half is not a number of blocks and truncating it loses the right-hand edge.
                EXPECT_EQ( across, ( width + block.Width - 1 ) / block.Width ) << "width " << width;
                EXPECT_GE( across * block.Width, width );
                EXPECT_LT( ( across - 1 ) * block.Width, width );

                EXPECT_EQ( Fmt::CalculateRowPitch( width, format ), across * block.Bytes );
                EXPECT_EQ( Fmt::CalculateImageSize( width, height, format ),
                           static_cast<uint64_t>( across ) * down * block.Bytes );
                EXPECT_EQ( Fmt::CalculateImageSize( width, height, format ) % block.Bytes, 0u )
                     << "a whole number of blocks is the only legal size";
            }
        }
    }
}

TEST( BlockCompression, TheSmallestMipsAreAWholeBlockAndThatIsWhereThisGoesWrong )
{
    // THE CASE THE BRIEF NAMED, and the one an engine that multiplies width by a bytes-per-pixel gets
    // wrong every time: a 1x1 level of a 4x4 block format is not one sixteenth of a block.
    for ( const Fmt::ImageFormat format : BlockFormats() )
    {
        const uint32_t blockBytes = Fmt::GetTexelBlock( format ).Bytes;
        for ( uint32_t extent = 1; extent <= 4; ++extent )
        {
            EXPECT_EQ( Fmt::CalculateImageSize( extent, extent, format ), blockBytes )
                 << "a " << extent << "x" << extent << " level of a block format is one whole block";
        }
        // And the resident tail of a real chain: 16, 8, 4, 2, 1 are 16, 4, 1, 1, 1 blocks.
        EXPECT_EQ( Fmt::CalculateImageSize( 16, 16, format ), 16u * blockBytes );
        EXPECT_EQ( Fmt::CalculateImageSize( 8, 8, format ), 4u * blockBytes );
        EXPECT_EQ( Fmt::CalculateImageSize( 4, 4, format ), 1u * blockBytes );
        EXPECT_EQ( Fmt::CalculateImageSize( 2, 2, format ), 1u * blockBytes );
        EXPECT_EQ( Fmt::CalculateImageSize( 1, 1, format ), 1u * blockBytes );
    }
}

TEST( BlockCompression, NoUncompressedFormatBecameABlockFormatByAccident )
{
    // The two questions the rest of the engine asks are kept in step here: a format with a 1x1 block has
    // a bytes-per-pixel and it IS the block's size, and a format with a bigger block has no answer at
    // all. The second half cannot be tested by calling it — the call is a deliberate abort — so what is
    // asserted is the predicate every caller branches on.
    for ( uint32_t i = 0; i < Fmt::kImageFormatCount; ++i )
    {
        const auto format = static_cast<Fmt::ImageFormat>( i );
        const auto block  = Fmt::GetTexelBlock( format );
        if ( Fmt::IsBlockCompressed( format ) )
        {
            EXPECT_TRUE( block.Width > 1 || block.Height > 1 );
        }
        else
        {
            EXPECT_EQ( block.Width, 1u );
            EXPECT_EQ( block.Height, 1u );
            EXPECT_EQ( Fmt::GetBytesPerPixel( format ), block.Bytes );
            EXPECT_EQ( Fmt::CalculateImageSize( 7, 5, format ), 7u * 5u * block.Bytes )
                 << "an uncompressed format's size must not have moved";
        }
    }
}

TEST( BlockCompression, ABlockFormatIsAQuarterOfItsSourceAndTheSourcePairingIsFixed )
{
    EXPECT_EQ( Fmt::BlockFormatFor( Fmt::ImageFormat::RGBA8F ), Fmt::ImageFormat::BC7_UNORM );
    EXPECT_EQ( Fmt::BlockFormatFor( Fmt::ImageFormat::RGBA32F ), Fmt::ImageFormat::BC6H_UFLOAT );
    // `Count` is the "no" — it is not a format, so it cannot be mistaken for one.
    EXPECT_EQ( Fmt::BlockFormatFor( Fmt::ImageFormat::RGBA16F ), Fmt::ImageFormat::Count );
    EXPECT_EQ( Fmt::BlockFormatFor( Fmt::ImageFormat::DEPTH32F ), Fmt::ImageFormat::Count );
    EXPECT_EQ( Fmt::BlockFormatFor( Fmt::ImageFormat::BC7_UNORM ), Fmt::ImageFormat::Count );

    EXPECT_EQ( Fmt::CalculateImageSize( 1024, 1024, Fmt::ImageFormat::BC7_UNORM ) * 4,
               Fmt::CalculateImageSize( 1024, 1024, Fmt::ImageFormat::RGBA8F ) );
    EXPECT_EQ( Fmt::CalculateImageSize( 1024, 1024, Fmt::ImageFormat::BC6H_UFLOAT ) * 16,
               Fmt::CalculateImageSize( 1024, 1024, Fmt::ImageFormat::RGBA32F ) );
}

// ── 2. THE PACKING ────────────────────────────────────────────────────────────────────────────────

TEST( BlockCompression, TheModeFieldIsWhereTheFormatSaysItIs )
{
    // A BPTC mode is written as m zero bits followed by a one, LSB first. Mode 6 is therefore 0x40 in
    // the first seven bits and mode 11 is 0x0F in the first five. If the bit writer ever counts the
    // other way these two bytes move, and every other check in this file would still pass by symmetry —
    // the encoder and the decoder here share the writer, so only a fixed expectation catches it.
    std::vector<unsigned char> ldr( 4 * 4 * 4, 0x20 );
    const auto bc7 = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                              ldr.data(), ldr.size() );
    ASSERT_TRUE( bc7.IsSuccess() ) << bc7.GetError();
    ASSERT_EQ( bc7.GetValue().size(), 16u );
    EXPECT_EQ( bc7.GetValue()[0] & 0x7F, 0x40 ) << "mode 6 is six zeroes then a one";

    std::vector<unsigned char> hdr( 4 * 4 * 16 );
    for ( std::size_t i = 0; i < 16; ++i )
        reinterpret_cast<float*>( hdr.data() )[i * 4 + 0] = 2.0f;
    const auto bc6 = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA32F, Fmt::ImageFormat::BC6H_UFLOAT,
                                              hdr.data(), hdr.size() );
    ASSERT_TRUE( bc6.IsSuccess() ) << bc6.GetError();
    ASSERT_EQ( bc6.GetValue().size(), 16u );
    // 0x03 AND NOT 0x0F, AND THIS LINE IS THE WHOLE REASON THIS TEST EXISTS. BC6H's mode field is a
    // CODE, not the mode's number: mode 11 is 0x03 and 0x0F is mode 14, a completely different endpoint
    // layout. The encoder wrote 0x0F for a while and its own decoder read it back the same way, so every
    // round trip in this file passed while no GPU on earth would have decoded the block — the baked
    // environment rendered pure white. A fixed expectation is the only check that can catch a mistake
    // both halves of a round trip make together.
    EXPECT_EQ( bc6.GetValue()[0] & 0x1F, 0x03 ) << "mode 11's five-bit code is 3";
}

// ── 3. THE RECONSTRUCTION ─────────────────────────────────────────────────────────────────────────

TEST( BlockCompression, AFlatBlockSurvivesExactly )
{
    // A one-point fit has no excuse: both endpoints land on the same colour, every index is 0, and the
    // sixteen texels come back bit for bit. This is the check that a wrong p-bit or a wrong endpoint
    // width fails immediately, because 7 bits plus a p-bit can express every one of 256 values EXACTLY
    // and 7 bits alone cannot express an odd one.
    for ( int value = 0; value < 256; ++value )
    {
        std::vector<unsigned char> source( 4 * 4 * 4, static_cast<unsigned char>( value ) );
        const auto blocks = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                                     source.data(), source.size() );
        ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
        const auto back = Fmt::BlockDecompressImage( 4, 4, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                                     blocks.GetValue().data(), blocks.GetValue().size() );
        ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
        EXPECT_EQ( back.GetValue(), source ) << "a flat block of " << value << " did not survive";
    }
}

TEST( BlockCompression, TheAnchorRuleIsObeyedAndDoesNotCostTheFirstTexel )
{
    // Index 0 is written in THREE bits, so the encoder has to guarantee it falls in the lower half of
    // the ramp — by exchanging the endpoints and mirroring every index when it does not. A block whose
    // first texel is the brightest is exactly the case that forces the swap, and the symptom of getting
    // it wrong is one wrong texel in sixteen: plausible, quiet, and in the corner.
    std::vector<unsigned char> source( 4 * 4 * 4 );
    for ( int t = 0; t < 16; ++t )
    {
        const unsigned char level = static_cast<unsigned char>( 250 - t * 15 );
        source[t * 4 + 0]         = level;
        source[t * 4 + 1]         = level;
        source[t * 4 + 2]         = level;
        source[t * 4 + 3]         = 255;
    }
    const auto blocks = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                                 source.data(), source.size() );
    ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
    // Bit 111 is the top bit of index 0: 7 mode + 56 endpoint + 2 p-bit = 65, plus the two low bits of
    // index 0 makes 67 the first bit that is NOT part of index 0. Rather than assert the bit, assert the
    // consequence: the brightest texel came back brightest.
    const auto back = Fmt::BlockDecompressImage( 4, 4, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                                 blocks.GetValue().data(), blocks.GetValue().size() );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_NEAR( back.GetValue()[0], 250, 4 ) << "texel 0 is the brightest and must decode as such";
    EXPECT_NEAR( back.GetValue()[15 * 4], 25, 4 ) << "texel 15 is the darkest";
}

TEST( BlockCompression, APartialBlockKeepsTheTexelsInsideTheImage )
{
    // A level whose extent is not a multiple of four is what the small end of every chain looks like,
    // and the texels outside the image take part in the endpoint fit. They are CLAMPED to the edge
    // rather than left at zero: a 5x3 image has eleven padding texels out of thirty-two, and if those
    // were black they would own the fit.
    // 201 AND NOT 200, ON PURPOSE: mode 6's p-bit is the low bit of all four channels of an endpoint at
    // once, so an opaque block can be exact only where the colour's parity matches alpha's. See
    // `AnOpaqueBlockKeepsItsAlphaAndPaysForItInColour` for the rule; this test is about the PADDING and
    // wants an image the encoder can reproduce exactly, so that a failure here means the padding leaked.
    const uint32_t             width = 5, height = 3;
    std::vector<unsigned char> source( static_cast<std::size_t>( width ) * height * 4 );
    for ( std::size_t i = 0; i < static_cast<std::size_t>( width ) * height; ++i )
    {
        source[i * 4 + 0] = 201;
        source[i * 4 + 1] = 201;
        source[i * 4 + 2] = 201;
        source[i * 4 + 3] = 255;
    }
    const auto blocks = Fmt::BlockCompressImage( width, height, Fmt::ImageFormat::RGBA8F,
                                                 Fmt::ImageFormat::BC7_UNORM, source.data(), source.size() );
    ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
    EXPECT_EQ( blocks.GetValue().size(), 2u * 16u ) << "5x3 is two blocks across and one down";

    const auto back =
         Fmt::BlockDecompressImage( width, height, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                    blocks.GetValue().data(), blocks.GetValue().size() );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue(), source ) << "clamped padding must not disturb a flat image";
}

TEST( BlockCompression, AnOpaqueBlockKeepsItsAlphaAndPaysForItInColour )
{
    // THE ONE PLACE THIS ENCODER DELIBERATELY DOES NOT MINIMISE SQUARED ERROR, so it is pinned rather
    // than left to be rediscovered as a defect.
    //
    // Mode 6 stores one p-bit per ENDPOINT, shared by all four of its channels, so an endpoint is
    // either all-even or all-odd in the reconstructed 8-bit values. (200,200,200,255) therefore cannot
    // be expressed: 200 wants even, 255 wants odd. Purely by squared error the even choice wins — one
    // channel off by one beats three — and every fully opaque texture in the project comes back with
    // alpha 254. The encoder fixes alpha's parity instead, and the colour pays the LSB.
    //
    // A LOWER NUMBER WOULD BE THE WRONG ANSWER HERE. Alpha is the channel a material may branch on, and
    // "opaque" is a fact rather than a shade; three colour channels about to be filtered, tonemapped and
    // quantized to eight bits are not.
    for ( const unsigned char colour : { 0, 1, 100, 200, 254 } )
    {
        std::vector<unsigned char> source( 4 * 4 * 4 );
        for ( int t = 0; t < 16; ++t )
        {
            source[t * 4 + 0] = colour;
            source[t * 4 + 1] = colour;
            source[t * 4 + 2] = colour;
            source[t * 4 + 3] = 255;
        }
        const auto blocks = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                                     source.data(), source.size() );
        ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
        const auto back = Fmt::BlockDecompressImage( 4, 4, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                                     blocks.GetValue().data(), blocks.GetValue().size() );
        ASSERT_TRUE( back.IsSuccess() ) << back.GetError();

        for ( int t = 0; t < 16; ++t )
        {
            EXPECT_EQ( back.GetValue()[t * 4 + 3], 255 )
                 << "an opaque block came back translucent at colour " << static_cast<int>( colour );
            for ( int c = 0; c < 3; ++c )
                EXPECT_LE( std::abs( static_cast<int>( back.GetValue()[t * 4 + c] ) - colour ), 1 )
                     << "the colour pays at most one LSB for alpha's parity";
        }
    }

    // And a fully TRANSPARENT block, which is the cut-out case and takes the other parity.
    std::vector<unsigned char> cutout( 4 * 4 * 4 );
    for ( int t = 0; t < 16; ++t )
    {
        cutout[t * 4 + 0] = 64;
        cutout[t * 4 + 1] = 64;
        cutout[t * 4 + 2] = 64;
        cutout[t * 4 + 3] = 0;
    }
    const auto blocks = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                                 cutout.data(), cutout.size() );
    ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
    const auto back = Fmt::BlockDecompressImage( 4, 4, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                                 blocks.GetValue().data(), blocks.GetValue().size() );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue(), cutout ) << "an even colour with alpha 0 shares a parity and is exact";
}

TEST( BlockCompression, HdrValuesSurviveAcrossTheWholeRange )
{
    // BC6H stores a HALF's bit pattern, not a number, so its accuracy is RELATIVE and roughly constant
    // across five orders of magnitude. That is the property that makes it right for radiance and it is
    // what is asserted: a flat block of any magnitude comes back within a fraction of a percent.
    for ( const float value : { 0.001f, 0.125f, 1.0f, 3.75f, 100.0f, 1200.0f, 60000.0f } )
    {
        std::vector<unsigned char> source( 4 * 4 * 16 );
        float*                     texels = reinterpret_cast<float*>( source.data() );
        for ( int t = 0; t < 16; ++t )
        {
            texels[t * 4 + 0] = value;
            texels[t * 4 + 1] = value * 0.5f;
            texels[t * 4 + 2] = value * 0.25f;
            texels[t * 4 + 3] = 1.0f;
        }
        const auto blocks = Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA32F,
                                                     Fmt::ImageFormat::BC6H_UFLOAT, source.data(), source.size() );
        ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
        const auto back =
             Fmt::BlockDecompressImage( 4, 4, Fmt::ImageFormat::BC6H_UFLOAT, Fmt::ImageFormat::RGBA32F,
                                        blocks.GetValue().data(), blocks.GetValue().size() );
        ASSERT_TRUE( back.IsSuccess() ) << back.GetError();

        const float* got = reinterpret_cast<const float*>( back.GetValue().data() );
        for ( int t = 0; t < 16; ++t )
        {
            EXPECT_NEAR( got[t * 4 + 0], value, value * 0.005f ) << "red at " << value;
            EXPECT_NEAR( got[t * 4 + 1], value * 0.5f, value * 0.005f ) << "green at " << value;
            EXPECT_NEAR( got[t * 4 + 2], value * 0.25f, value * 0.005f ) << "blue at " << value;
            EXPECT_FLOAT_EQ( got[t * 4 + 3], 1.0f ) << "BC6H has no alpha; the decoder writes 1";
        }
    }
}

TEST( BlockCompression, ARefusalNamesTheShapeItWasHandedRatherThanReadingPastIt )
{
    std::vector<unsigned char> source( 4 * 4 * 4 );
    EXPECT_FALSE( Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                           source.data(), source.size() - 1 )
                       .IsSuccess() )
         << "a short buffer must be refused, not read past";
    EXPECT_FALSE( Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC6H_UFLOAT,
                                           source.data(), source.size() )
                       .IsSuccess() )
         << "RGBA8 does not encode into BC6H";
    EXPECT_FALSE(
         Fmt::BlockCompressImage( 0, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM, source.data(), 0 )
              .IsSuccess() );
    EXPECT_FALSE( Fmt::BlockCompressImage( 4, 4, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM, nullptr,
                                           source.size() )
                       .IsSuccess() );
}

// ── 4. THE CORPUS REGISTER ────────────────────────────────────────────────────────────────────────

namespace
{
    /// The cook's two gates, repeated here because this suite is what would notice them moving. They are
    /// not `#include`d from the importer: that lives in the Editor target and this suite deliberately
    /// links neither it nor the renderer. A disagreement between the two numbers is caught by the
    /// verdicts below changing, which is the thing that matters.
    constexpr double   kPsnrFloorDb     = 45.0;
    constexpr uint32_t kMaxDeltaCeiling = 32;

    enum class Verdict
    {
        /// The cook keeps the blocks: the image clears both gates.
        Compressed,
        /// The cook keeps the pixels: `Docs/Textures/T1_BCN_MEASUREMENT.md`'s rules, reached by
        /// measurement rather than by an authored field that does not exist.
        RefusedForQuality,
    };

    struct CorpusRow
    {
        const char* Path;
        Verdict     Expected;
        const char* Why;
    };

    // ONE ROW PER SOURCE IMAGE, and the verdict rather than the number. A register pinned to "52.46 dB"
    // is satisfied by editing 52.46; a register pinned to "texture_diffuse is compressed and
    // texture_normal is not" can only be satisfied by an encoder that still tells them apart.
    // clang-format off
    const std::vector<CorpusRow>& Corpus()
    {
        static const std::vector<CorpusRow> rows = {
        { "Editor/Resources/Assets/Textures/T_Checker.png", Verdict::Compressed,
          "the one texture actually referenced by shipped scenes (M_CheckerFloor, 41 of them); flat "
          "colours in large runs, which is the easy case for a single-subset mode" },
        { "Editor/Resources/Assets/Textures/1k_Dissolve_Noise_Texture.png", Verdict::RefusedForQuality,
          "NOISE. T1 measured 29.29 dB for BC4 on it and called it the one substitution visible in a "
          "frame; this encoder reaches 37.8 dB, which is better and still the worst in the corpus. It "
          "fails the PSNR gate and passes the delta gate, which is why there are two" },
        { "Editor/Resources/Assets/Meshes/texture_diffuse.png", Verdict::Compressed,
          "albedo, the case BC7 exists for; T1 measured 52.35 dB with a reference encoder and this one "
          "reaches 52.46 on the same file" },
        { "Editor/Resources/Assets/Meshes/texture_normal.png", Verdict::RefusedForQuality,
          "A NORMAL MAP, and T1's most load-bearing rule: BC7 on normals is WORSE than BC5 and 106x "
          "more expensive. Measured here at 46.46 dB against T1's 46.48 for a reference encoder -- and "
          "a worst texel off by 131, which is what the delta gate is for" },
        { "Editor/Resources/Assets/Meshes/texture_pbr.png", Verdict::RefusedForQuality,
          "a PACKED map -- three unrelated channels in one image, so no single line through RGB can fit "
          "them. Its average is respectable and its worst texel is off by 84" },
        { "Editor/Resources/Assets/Meshes/texture_roughness.png", Verdict::Compressed,
          "a single-channel mask replicated across RGB; T1 would rather have BC4 here (twice the "
          "saving), and that needs the authored field this step does not have" },
        { "Editor/Resources/Assets/Meshes/texture_metallic.png", Verdict::Compressed,
          "the same shape as roughness and the easiest image in the corpus" },
        { "Editor/Resources/Assets/Meshes/shaded.png", Verdict::Compressed,
          "baked shading, smooth gradients" },
        { "Editor/Resources/Assets/Clouds/Layouts/O4_MaskAddRemove.png", Verdict::Compressed,
          "a cloud layout mask; the alpha channel carries the signal and BC7 is the only format here "
          "with a full-precision alpha at all" },
        { "Editor/Resources/Assets/Clouds/Layouts/PTP_LetterP.png", Verdict::Compressed,
          "two-colour artwork, which a single subset reproduces exactly" },
        };
        return rows;
    }
    // clang-format on

    struct Fidelity
    {
        double   Psnr             = 0.0;
        uint32_t MaxAbsoluteDelta = 0;
        bool     Read             = false;
    };

    Fidelity MeasureFile( const std::string& path )
    {
        int      width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load( path.c_str(), &width, &height, &channels, 4 );
        if ( pixels == nullptr )
            return {};

        const std::vector<unsigned char> source( pixels, pixels + static_cast<std::size_t>( width ) * height * 4 );
        stbi_image_free( pixels );

        const auto blocks = Fmt::BlockCompressImage( static_cast<uint32_t>( width ),
                                                     static_cast<uint32_t>( height ), Fmt::ImageFormat::RGBA8F,
                                                     Fmt::ImageFormat::BC7_UNORM, source.data(), source.size() );
        if ( !blocks.IsSuccess() )
            return {};
        const auto back = Fmt::BlockDecompressImage(
             static_cast<uint32_t>( width ), static_cast<uint32_t>( height ), Fmt::ImageFormat::BC7_UNORM,
             Fmt::ImageFormat::RGBA8F, blocks.GetValue().data(), blocks.GetValue().size() );
        if ( !back.IsSuccess() )
            return {};

        Fidelity fidelity;
        fidelity.Read     = true;
        double squaredSum = 0.0;
        for ( std::size_t i = 0; i < source.size(); ++i )
        {
            const int delta = static_cast<int>( source[i] ) - static_cast<int>( back.GetValue()[i] );
            squaredSum += static_cast<double>( delta ) * delta;
            fidelity.MaxAbsoluteDelta =
                 std::max( fidelity.MaxAbsoluteDelta, static_cast<uint32_t>( std::abs( delta ) ) );
        }
        const double meanSquared = squaredSum / static_cast<double>( source.size() );
        fidelity.Psnr            = meanSquared <= 0.0 ? 1000.0 : 10.0 * std::log10( 65025.0 / meanSquared );
        return fidelity;
    }
} // namespace

TEST( BlockCompression, EverySourceImageReachesTheVerdictItsRowRecords )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "the corpus cannot find the repository from the working directory";

    int compressed = 0, refused = 0;
    for ( const CorpusRow& row : Corpus() )
    {
        const Fidelity fidelity = MeasureFile( root + row.Path );
        ASSERT_TRUE( fidelity.Read ) << row.Path << " could not be read or encoded";

        const bool    keeps   = fidelity.Psnr >= kPsnrFloorDb && fidelity.MaxAbsoluteDelta <= kMaxDeltaCeiling;
        const Verdict reached = keeps ? Verdict::Compressed : Verdict::RefusedForQuality;

        EXPECT_EQ( reached, row.Expected )
             << "\n  " << row.Path << "\n  " << fidelity.Psnr << " dB, worst texel off by "
             << fidelity.MaxAbsoluteDelta << " (floor " << kPsnrFloorDb << " dB, ceiling " << kMaxDeltaCeiling
             << ")\n  the row says: " << row.Why;

        ( row.Expected == Verdict::Compressed ? compressed : refused )++;

        std::cout << "[  CORPUS  ] " << ( keeps ? "BC7      " : "refused  " ) << fidelity.Psnr << " dB  max|d| "
                  << fidelity.MaxAbsoluteDelta << "  " << row.Path << "\n";
    }

    // DERIVED FROM THE ROWS. Both verdicts have to occur, or the register is not telling anything apart.
    EXPECT_EQ( compressed + refused, static_cast<int>( Corpus().size() ) );
    EXPECT_GT( compressed, 0 );
    EXPECT_GT( refused, 0 );
}

TEST( BlockCompression, TheCorpusRegisterDescribesFilesThatStillExist )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    for ( const CorpusRow& row : Corpus() )
    {
        EXPECT_TRUE( std::filesystem::exists( root + row.Path ) )
             << row.Path
             << " has a row and is not in the tree; a register that keeps rows for files that "
                "left stops being a description of the tree.";
        EXPECT_GT( std::string( row.Why ).size(), 30u ) << row.Path << " has a row but not an argument";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
