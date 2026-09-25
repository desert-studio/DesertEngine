// THE SPLASH'S LIVE LAYER, WITHOUT A WINDOW: where each element goes, what the counter says, and that
// the picture loader hands back what the cook wrote. The two native implementations draw from exactly
// these functions, so a number pinned here is a number both platforms put on screen.

#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashLayout.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Splash = Desert::Editor::Splash;

// --- The layout -------------------------------------------------------------------------------------

TEST( SplashLayout, TheElementsSitWhereTheDesignPutsThem )
{
    const Splash::Layout layout = Splash::ComputeLayout( 0.0 );
    EXPECT_EQ( layout.Project.X, 48.0f );
    EXPECT_EQ( layout.Project.Y, 70.0f );
    EXPECT_EQ( layout.Stage.X, 48.0f );
    EXPECT_EQ( layout.Stage.Y, 52.0f );
    EXPECT_EQ( layout.Item.X, 48.0f );
    EXPECT_EQ( layout.Item.Y, 36.0f );
    // The item line has no right-hand neighbour and runs margin to margin.
    EXPECT_EQ( layout.Item.X + layout.Item.W, Splash::kWidth - 48.0f );
    EXPECT_EQ( layout.BarTrack.Y, 22.0f );
    EXPECT_EQ( layout.BarTrack.H, 2.0f );
    // The bar has the same 48-point margin on both sides.
    EXPECT_EQ( layout.BarTrack.X, 48.0f );
    EXPECT_EQ( layout.BarTrack.X + layout.BarTrack.W, Splash::kWidth - 48.0f );
    // The right-aligned column ends on the same margin.
    EXPECT_EQ( layout.Percent.X + layout.Percent.W, Splash::kWidth - 48.0f );
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
    // The stage label and the percentage share a line and each owns a half of it; the project and the
    // version likewise. A label that grew under the percentage would be unreadable on both.
    const Splash::Layout layout = Splash::ComputeLayout( 0.5 );
    EXPECT_LE( layout.Stage.X + layout.Stage.W, layout.Percent.X );
    EXPECT_LE( layout.Project.X + layout.Project.W, layout.Version.X );
    EXPECT_EQ( layout.Percent.Y, layout.Stage.Y );
    // The lines stack without touching: bar, item, stage, project — each box above the one below.
    EXPECT_GE( layout.Item.Y, layout.BarTrack.Y + layout.BarTrack.H );
    EXPECT_GE( layout.Stage.Y, layout.Item.Y + layout.Item.H );
    EXPECT_GE( layout.Project.Y, layout.Stage.Y + layout.Stage.H );
}

TEST( SplashLayout, NoLiveLineReachesThePicturesEngineWordmark )
{
    // The live lines are drawn over a picture that already has text in it. The project line used to sit
    // at y 80 with its box ending 1.3 points under ENGINE's glyphs, and on screen the two read as one line.
    const Splash::Layout layout = Splash::ComputeLayout( 0.5 );
    const float          floor  = Splash::kWordmarkEngine.Y - Splash::kWordmarkClearance;
    for ( const Splash::Rect& box : { layout.Project, layout.Version, layout.Stage, layout.Percent, layout.Item } )
        EXPECT_LE( box.Y + box.H, floor ) << "a text box at y " << box.Y << " reaches the wordmark";
}

TEST( SplashLayout, TheWordmarkBoxIsWhereThePictureDrawsEngine )
{
    // THE CONSTANT AGAINST THE PIXELS: every sand-coloured pixel in the lower-left of the committed
    // picture (ENGINE's glyphs; DESERT is white) lies inside kWordmarkEngine, and the box is not empty.
    // A re-baked picture that moved the wordmark fails here instead of under the project line.
    const std::filesystem::path repo =
         std::filesystem::path( __FILE__ ).parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto loaded = Splash::LoadSplashPixels( repo / "Editor" / Splash::kSplashTexture );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    const Splash::SplashPixels& picture = loaded.GetValue();
    const float                 scale   = static_cast<float>( picture.Width ) / Splash::kWidth;

    float left = Splash::kWidth, right = 0.0f, bottom = Splash::kHeight, top = 0.0f;
    // Rows and columns in design points: x 40..420, y 30..140 from the bottom — the whole column the live
    // lines use, so a wordmark that moved down into it is seen.
    for ( uint32_t row = static_cast<uint32_t>( ( Splash::kHeight - 140.0f ) * scale );
          row < static_cast<uint32_t>( ( Splash::kHeight - 30.0f ) * scale ); ++row )
        for ( uint32_t col = static_cast<uint32_t>( 40.0f * scale ); col < static_cast<uint32_t>( 420.0f * scale );
              ++col )
        {
            const unsigned char* p = &picture.Rgba[( static_cast<std::size_t>( row ) * picture.Width + col ) * 4];
            const bool           sand = p[0] > 215 && p[1] > 150 && p[1] < 215 && p[2] < 170 && p[0] - p[2] > 70;
            if ( !sand )
                continue;
            const float x = static_cast<float>( col ) / scale;
            const float y = Splash::kHeight - static_cast<float>( row ) / scale;
            left          = std::min( left, x );
            right         = std::max( right, x );
            bottom        = std::min( bottom, y );
            top           = std::max( top, y );
        }
    ASSERT_LT( left, right ) << "no ENGINE glyph found in the picture";
    const Splash::Rect& box = Splash::kWordmarkEngine;
    EXPECT_GE( left, box.X - 1.0f );
    EXPECT_LE( right, box.X + box.W + 1.0f );
    EXPECT_GE( bottom, box.Y - 1.0f );
    EXPECT_LE( top, box.Y + box.H + 1.0f );
    // And the box is tight, so the clearance above is a clearance from the glyphs, not from slack.
    EXPECT_LE( bottom, box.Y + 2.0f );
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

// --- The motion -------------------------------------------------------------------------------------

TEST( SplashLayout, TheFadesTakeAtMostTwoHundredMilliseconds )
{
    EXPECT_LE( Splash::kFadeInSeconds, 0.2f );
    EXPECT_LE( Splash::kFadeOutSeconds, 0.2f );
    EXPECT_EQ( Splash::SplashOpacity( 0.0f, false ), 0.0f );
    EXPECT_EQ( Splash::SplashOpacity( Splash::kFadeInSeconds, false ), 1.0f );
    EXPECT_EQ( Splash::SplashOpacity( 0.0f, true ), 1.0f );
    EXPECT_EQ( Splash::SplashOpacity( Splash::kFadeOutSeconds, true ), 0.0f );
    EXPECT_FLOAT_EQ( Splash::SplashOpacity( Splash::kFadeInSeconds * 0.5f, false ), 0.5f );
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
        std::ofstream( file, std::ios::binary )
             .write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        return file;
    }

    // A whole mip chain, as the cook writes one: the container refuses a partial chain, and a test that
    // wrote one level would be testing a file the cook never produces.
    Ser::TextureAssetData Cooked( const Fmt::ImageFormat format )
    {
        Ser::TextureAssetData data;
        data.Width  = kSide;
        data.Height = kSide;
        data.Format = Fmt::ImageFormat::RGBA8F;

        std::vector<unsigned char> rgbaChain;
        auto levels = Ser::BuildMipChain( kSide, kSide, Fmt::ImageFormat::RGBA8F, Gradient(), rgbaChain );
        EXPECT_TRUE( levels.IsSuccess() );
        data.Levels = levels.GetValue();
        data.Pixels = rgbaChain;
        if ( format == Fmt::ImageFormat::RGBA8F )
            return data;

        const auto count  = static_cast<uint32_t>( data.Levels.size() );
        auto       blocks = Ser::BlockCompressChain( kSide, kSide, count, 1, Fmt::ImageFormat::RGBA8F, format,
                                                     data.Levels, data.Pixels );
        EXPECT_TRUE( blocks.IsSuccess() );
        std::vector<unsigned char> blockChain;
        auto table = Ser::BuildLevelTable( kSide, kSide, count, 1, format, blocks.GetValue(), blockChain );
        EXPECT_TRUE( table.IsSuccess() );
        data.Format = format;
        data.Levels = table.GetValue();
        data.Pixels = blockChain;
        return data;
    }
} // namespace

TEST( SplashLayout, TheLoaderDecodesTheCooksBC7ToTheSameTexelsTheReferenceDecoderGives )
{
    const Ser::TextureAssetData cooked = Cooked( Fmt::ImageFormat::BC7_UNORM );
    ASSERT_FALSE( cooked.Levels.empty() );
    const auto expected =
         Fmt::BlockDecompressImage( kSide, kSide, Fmt::ImageFormat::BC7_UNORM, Fmt::ImageFormat::RGBA8F,
                                    cooked.Pixels.data() + cooked.Levels[0].ByteOffset,
                                    static_cast<std::size_t>( cooked.Levels[0].ByteSize ) );
    ASSERT_TRUE( expected.IsSuccess() );

    const auto file   = WriteTex( "bc7.tex", cooked );
    const auto loaded = Splash::LoadSplashPixels( file );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue().Width, kSide );
    EXPECT_EQ( loaded.GetValue().Height, kSide );
    EXPECT_EQ( loaded.GetValue().Rgba, expected.GetValue() );
    // And it is the picture, not a flat block: both ramps survive, left-to-right in red and
    // top-to-bottom in green (a 2-D ramp is not on one line in colour space, so BC7 mode 6 bends it by
    // a couple of dozen levels at a block's corners; the direction is what is asserted, not the level).
    const auto& rgba = loaded.GetValue().Rgba;
    EXPECT_GT( rgba[( 0 * kSide + 15 ) * 4], rgba[( 0 * kSide + 0 ) * 4] + 150 );
    EXPECT_GT( rgba[( 15 * kSide + 0 ) * 4 + 1], rgba[( 0 * kSide + 0 ) * 4 + 1] + 150 );
}

TEST( SplashLayout, AnUncompressedCookIsTakenAsItIs )
{
    const auto source = Gradient();
    const auto file   = WriteTex( "rgba8.tex", Cooked( Fmt::ImageFormat::RGBA8F ) );
    const auto loaded = Splash::LoadSplashPixels( file );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue().Rgba, source );
}

TEST( SplashLayout, AMissingPictureIsARefusalThatNamesTheFileAndWhatMakesIt )
{
    // The picture is a COMMITTED engine resource now (owner, 2026-09-23), so a missing one is a broken
    // install, not a first start: the sentence names the file and the tool that regenerates it.
    const auto missing = stdfs::temp_directory_path() / "SplashLayoutSuite" / "no-such-splash.tex";
    const auto loaded  = Splash::LoadSplashPixels( missing );
    ASSERT_FALSE( loaded.IsSuccess() );
    EXPECT_NE( loaded.GetError().find( "no-such-splash.tex" ), std::string::npos ) << loaded.GetError();
    EXPECT_NE( loaded.GetError().find( "TextureCook" ), std::string::npos ) << loaded.GetError();
}

TEST( SplashLayout, TheRepositoryCarriesAPictureTheSplashCanDraw )
{
    // THE RELATION, NOT THE FILE: the committed .tex must decode to the size of the source it was made
    // from. A regenerated Splash.jpg without a regenerated Splash.tex (or the reverse) fails here, and
    // the first start of every fresh clone would otherwise show a stale or missing picture.
    const stdfs::path repo =
         stdfs::path( __FILE__ ).parent_path().parent_path().parent_path().parent_path().parent_path();
    const stdfs::path picture = repo / "Editor" / Splash::kSplashTexture;
    const auto        loaded  = Splash::LoadSplashPixels( picture );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue().Width, 2400u );
    EXPECT_EQ( loaded.GetValue().Height, 1350u );
}

TEST( SplashLayout, AFileThatIsNotACookedTextureIsRefusedNotDrawn )
{
    const auto file = stdfs::temp_directory_path() / "SplashLayoutSuite" / "garbage.tex";
    std::ofstream( file, std::ios::binary ) << "this is a jpeg, honestly";
    EXPECT_FALSE( Splash::LoadSplashPixels( file ).IsSuccess() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
