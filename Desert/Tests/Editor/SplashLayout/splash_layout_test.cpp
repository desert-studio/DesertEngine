// THE SPLASH'S LIVE LAYER, WITHOUT A WINDOW: where each element goes, what the counter says, and that
// the picture loader hands back what the cook wrote. The two native implementations draw from exactly
// these functions, so a number pinned here is a number both platforms put on screen.

#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashLayout.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace Splash = Desert::Editor::Splash;

// --- The counter ------------------------------------------------------------------------------------

TEST( SplashLayout, TheCounterNamesTheStepBeingRunAndTheShareAlreadyDone )
{
    EXPECT_EQ( Splash::FormatProgress( 0, 17 ), "1 / 17   0%" );
    EXPECT_EQ( Splash::FormatProgress( 8, 17 ), "9 / 17   47%" );
    // The last step reads M / M, and the bar does not yet claim it is finished.
    EXPECT_EQ( Splash::FormatProgress( 16, 17 ), "17 / 17   94%" );
}

TEST( SplashLayout, APlanNotKnownYetDrawsNoCounterAndAnEmptyBar )
{
    // Before the editor has built its stage list (the renderer is still coming up) there is nothing
    // honest to count; "1 / 0" or "0 / 0   0%" would be a number that says nothing.
    EXPECT_EQ( Splash::FormatProgress( 0, 0 ), "" );
    EXPECT_EQ( Splash::ProgressFraction( 0, 0 ), 0.0 );
    EXPECT_EQ( Splash::ComputeLayout( Splash::ProgressFraction( 0, 0 ) ).BarFill.W, 0.0f );
}

TEST( SplashLayout, AnIndexPastTheEndIsClampedRatherThanOverdrawn )
{
    EXPECT_EQ( Splash::FormatProgress( 40, 17 ), "17 / 17   100%" );
    EXPECT_EQ( Splash::ProgressFraction( 40, 17 ), 1.0 );
}

// --- The layout -------------------------------------------------------------------------------------

TEST( SplashLayout, TheElementsSitWhereTheDesignPutsThem )
{
    const Splash::Layout layout = Splash::ComputeLayout( 0.0 );
    EXPECT_EQ( layout.Project.X, 48.0f );
    EXPECT_EQ( layout.Project.Y, 58.0f );
    EXPECT_EQ( layout.Stage.X, 48.0f );
    EXPECT_EQ( layout.Stage.Y, 36.0f );
    EXPECT_EQ( layout.BarTrack.Y, 22.0f );
    EXPECT_EQ( layout.BarTrack.H, 2.0f );
    // The bar has the same 48-point margin on both sides.
    EXPECT_EQ( layout.BarTrack.X, 48.0f );
    EXPECT_EQ( layout.BarTrack.X + layout.BarTrack.W, Splash::kWidth - 48.0f );
    // The right-aligned column ends on the same margin.
    EXPECT_EQ( layout.Counter.X + layout.Counter.W, Splash::kWidth - 48.0f );
    EXPECT_EQ( layout.Version.X + layout.Version.W, Splash::kWidth - 48.0f );
}

TEST( SplashLayout, TheFillIsTheTrackTimesTheFractionFromTheSameOrigin )
{
    for ( const double fraction : { 0.0, 0.25, 0.5, 1.0 } )
    {
        const Splash::Layout layout = Splash::ComputeLayout( fraction );
        EXPECT_EQ( layout.BarFill.X, layout.BarTrack.X );
        EXPECT_EQ( layout.BarFill.Y, layout.BarTrack.Y );
        EXPECT_EQ( layout.BarFill.H, layout.BarTrack.H );
        EXPECT_FLOAT_EQ( layout.BarFill.W, static_cast<float>( layout.BarTrack.W * fraction ) );
    }
    // Out of range is clamped: a fill wider than its track would run off the right margin.
    EXPECT_EQ( Splash::ComputeLayout( 1.7 ).BarFill.W, Splash::ComputeLayout( 1.0 ).BarFill.W );
    EXPECT_EQ( Splash::ComputeLayout( -0.3 ).BarFill.W, 0.0f );
}

TEST( SplashLayout, NoTwoTextBoxesOverlap )
{
    // The stage label and the counter share a line and each owns a half of it; the project and the
    // version likewise. A label that grew under the counter would be unreadable on both.
    const Splash::Layout layout = Splash::ComputeLayout( 0.5 );
    EXPECT_LE( layout.Stage.X + layout.Stage.W, layout.Counter.X );
    EXPECT_LE( layout.Project.X + layout.Project.W, layout.Version.X );
    // And the text sits above the bar, not on it.
    EXPECT_GE( layout.Stage.Y, layout.BarTrack.Y + layout.BarTrack.H );
    EXPECT_GE( layout.Project.Y, layout.Stage.Y + layout.Stage.H );
}

TEST( SplashLayout, FlippingToATopOriginKeepsTheBoxAndMovesItsReferenceEdge )
{
    const Splash::Rect bar     = Splash::ComputeLayout( 0.0 ).BarTrack;
    const Splash::Rect flipped = Splash::FlipY( bar );
    EXPECT_EQ( flipped.X, bar.X );
    EXPECT_EQ( flipped.W, bar.W );
    EXPECT_EQ( flipped.H, bar.H );
    // 22 points from the bottom of a 675-point splash, 2 points tall: its top is 651 from the top.
    EXPECT_EQ( flipped.Y, 651.0f );
    EXPECT_EQ( Splash::FlipY( flipped ).Y, bar.Y );
}

TEST( SplashLayout, TheDesignShrinksToFitASmallScreenAndNeverGrows )
{
    EXPECT_EQ( Splash::FitScale( 3000.0f, 2000.0f ), 1.0f );
    // 1280x720 usable: 80 % of it is 1024 wide, so 1024 / 1200.
    EXPECT_FLOAT_EQ( Splash::FitScale( 1280.0f, 720.0f ), 1024.0f / 1200.0f );
    // A tall narrow screen is bound by its width, a wide short one by its height.
    EXPECT_FLOAT_EQ( Splash::FitScale( 1000.0f, 4000.0f ), 800.0f / 1200.0f );
    EXPECT_FLOAT_EQ( Splash::FitScale( 4000.0f, 500.0f ), 400.0f / 675.0f );
    // No screen information is not a reason to draw nothing.
    EXPECT_EQ( Splash::FitScale( 0.0f, 0.0f ), 1.0f );
}

// --- The picture ------------------------------------------------------------------------------------

namespace
{
    namespace Fmt   = Desert::Core::Formats;
    namespace Ser   = Desert::Assets::Serialization;
    namespace stdfs = std::filesystem;

    constexpr uint32_t kSide = 16;

    std::vector<unsigned char> Gradient()
    {
        std::vector<unsigned char> rgba( kSide * kSide * 4 );
        for ( uint32_t y = 0; y < kSide; ++y )
            for ( uint32_t x = 0; x < kSide; ++x )
            {
                unsigned char* p = &rgba[( y * kSide + x ) * 4];
                p[0]             = static_cast<unsigned char>( x * 16 );
                p[1]             = static_cast<unsigned char>( y * 16 );
                p[2]             = 128;
                p[3]             = 255;
            }
        return rgba;
    }

    stdfs::path WriteTex( const std::string& name, const Ser::TextureAssetData& data )
    {
        const stdfs::path dir = stdfs::temp_directory_path() / "SplashLayoutSuite";
        stdfs::create_directories( dir );
        const stdfs::path file  = dir / name;
        const std::string bytes = Ser::EncodeTextureBinary( data );
        std::ofstream( file, std::ios::binary ).write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        return file;
    }

    Ser::TextureAssetData OneLevel( const Fmt::ImageFormat format, std::vector<unsigned char> pixels,
                                    const uint32_t rowPitch )
    {
        Ser::TextureAssetData data;
        data.Width  = kSide;
        data.Height = kSide;
        data.Format = format;
        data.Levels = { Ser::TextureLevel{ kSide, kSide, 0, pixels.size(), rowPitch } };
        data.Pixels = std::move( pixels );
        return data;
    }
} // namespace

TEST( SplashLayout, TheLoaderDecodesTheCooksBC7ToTheSameTexelsTheReferenceDecoderGives )
{
    const auto source = Gradient();
    auto blocks = Fmt::BlockCompressImage( kSide, kSide, Fmt::ImageFormat::RGBA8F, Fmt::ImageFormat::BC7_UNORM,
                                           source.data(), source.size() );
    ASSERT_TRUE( blocks.IsSuccess() ) << blocks.GetError();
    const auto expected = Fmt::BlockDecompressImage( kSide, kSide, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                                     blocks.GetValue().data(), blocks.GetValue().size() );
    ASSERT_TRUE( expected.IsSuccess() );

    const auto file =
         WriteTex( "bc7.tex", OneLevel( Fmt::ImageFormat::BC7_UNORM, blocks.ExtractValue(), ( kSide / 4 ) * 16 ) );
    const auto loaded = Splash::LoadSplashPixels( file );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue().Width, kSide );
    EXPECT_EQ( loaded.GetValue().Height, kSide );
    EXPECT_EQ( loaded.GetValue().Rgba, expected.GetValue() );
    // And it is the picture, not a flat block: the gradient survives the round trip within BC7's error.
    EXPECT_NEAR( loaded.GetValue().Rgba[( 0 * kSide + 15 ) * 4], 240, 8 );
    EXPECT_NEAR( loaded.GetValue().Rgba[( 15 * kSide + 0 ) * 4 + 1], 240, 8 );
}

TEST( SplashLayout, AnUncompressedCookIsTakenAsItIs )
{
    const auto source = Gradient();
    const auto file   = WriteTex( "rgba8.tex", OneLevel( Fmt::ImageFormat::RGBA8F, source, kSide * 4 ) );
    const auto loaded = Splash::LoadSplashPixels( file );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue().Rgba, source );
}

TEST( SplashLayout, AMissingCookIsARefusalThatNamesTheFileAndWhoWritesIt )
{
    // The first start on a fresh clone has no `.tex` yet. The splash draws on its plain background and
    // logs THIS sentence, so it has to say which file and how it comes to exist.
    const auto missing = stdfs::temp_directory_path() / "SplashLayoutSuite" / "no-such-splash.tex";
    const auto loaded  = Splash::LoadSplashPixels( missing );
    ASSERT_FALSE( loaded.IsSuccess() );
    EXPECT_NE( loaded.GetError().find( "no-such-splash.tex" ), std::string::npos ) << loaded.GetError();
    EXPECT_NE( loaded.GetError().find( "Package.sh" ), std::string::npos ) << loaded.GetError();
}

TEST( SplashLayout, AFileThatIsNotACookedTextureIsRefusedNotDrawn )
{
    const auto file = stdfs::temp_directory_path() / "SplashLayoutSuite" / "garbage.tex";
    std::ofstream( file, std::ios::binary ) << "this is a jpeg, honestly";
    EXPECT_FALSE( Splash::LoadSplashPixels( file ).IsSuccess() );
}
