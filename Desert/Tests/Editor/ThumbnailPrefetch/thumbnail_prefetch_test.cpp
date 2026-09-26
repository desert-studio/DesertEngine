// "A cached thumbnail is decoded on a worker before it is drawn; nothing sweeps the project for captures."
//
// TH2 (owner: "as in UE"). ThumbnailPrefetch decodes PNGs that are already in the disk cache on a JobSystem
// worker — during the splash, for the folder the Content Browser opens on — so the first frame after the
// window is shown only uploads. Captures stay lazy: only a tile being drawn asks for one, and the
// project-wide background sweep is gone (decision В4).

#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailPrefetch.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert::Editor;
namespace fs = std::filesystem;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Widgets/ThumbnailService.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    constexpr std::array<unsigned char, 70> kOnePixelPng = {
         0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
         0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
         0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
         0xda, 0x63, 0x64, 0x60, 0xf8, 0x5f, 0x0f, 0x00, 0x02, 0x87, 0x01, 0x80, 0xeb, 0x47,
         0xba, 0x92, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

    struct Fixture
    {
        fs::path Dir;
        fs::path Source;
        fs::path Png;

        Fixture()
        {
            Dir = fs::temp_directory_path() /
                  ( "th2_prefetch_" + std::to_string( ::testing::UnitTest::GetInstance()->random_seed() ) +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() );
            fs::remove_all( Dir );
            fs::create_directories( Dir );
            Source = Dir / "M_Test.demat";
            Png    = Dir / "M_Test.png";
            std::ofstream( Source ) << "{ \"material\": 1 }";
            ThumbnailPrefetch::Get().Clear();
        }
        ~Fixture()
        {
            ThumbnailPrefetch::Get().Clear();
            std::error_code ec;
            fs::remove_all( Dir, ec );
        }
        void WriteFreshPng() const
        {
            std::ofstream out( Png, std::ios::binary );
            out.write( reinterpret_cast<const char*>( kOnePixelPng.data() ), kOnePixelPng.size() );
            out.close();
            const auto hash = ThumbnailFreshness::ContentHash( Source );
            ASSERT_TRUE( hash.has_value() );
            ASSERT_TRUE( ThumbnailFreshness::Record( Png, *hash ) );
        }
    };
} // namespace

TEST( ThumbnailPrefetch, ACachedPngIsDecodedOnAWorkerBeforeAnyoneDrawsIt )
{
    Fixture f;
    f.WriteFreshPng();

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), f.Source.string() } } );
    ThumbnailPrefetch::Get().Drain();
    ASSERT_EQ( ThumbnailPrefetch::Get().ReadyCount(), 1u );

    const auto pixels = ThumbnailPrefetch::Get().Take( f.Png.string(), fs::last_write_time( f.Png ) );
    ASSERT_TRUE( pixels.has_value() ) << "the cached PNG was not decoded ahead of the draw";
    EXPECT_EQ( pixels->Width, 1 );
    EXPECT_EQ( pixels->Height, 1 );
    EXPECT_EQ( pixels->Rgba.size(), 4u );
}

TEST( ThumbnailPrefetch, NoPngOnDiskMeansNothingIsDecodedAndNothingIsCaptured )
{
    Fixture f; // no PNG: the capture is owed, and the capture queue waits for the window

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), f.Source.string() } } );
    ThumbnailPrefetch::Get().Drain();
    EXPECT_FALSE( ThumbnailPrefetch::Get().Take( f.Png.string(), {} ).has_value() );
}

TEST( ThumbnailPrefetch, AFileRewrittenSinceTheDecodeIsNotHandedOver )
{
    Fixture f;
    f.WriteFreshPng();

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), f.Source.string() } } );
    ThumbnailPrefetch::Get().Drain();
    const auto later = fs::last_write_time( f.Png ) + std::chrono::seconds( 5 );
    EXPECT_FALSE( ThumbnailPrefetch::Get().Take( f.Png.string(), later ).has_value() )
         << "pixels of an older file would be uploaded as the picture of the new one";
}

TEST( ThumbnailPrefetch, AUsersOwnImageNeedsNoFreshnessRecord )
{
    Fixture       f;
    std::ofstream out( f.Png, std::ios::binary );
    out.write( reinterpret_cast<const char*>( kOnePixelPng.data() ), kOnePixelPng.size() );
    out.close();

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), {} } } );
    ThumbnailPrefetch::Get().Drain();
    EXPECT_TRUE( ThumbnailPrefetch::Get().Take( f.Png.string(), fs::last_write_time( f.Png ) ).has_value() );
}

// Decision В4: no project-wide sweep. Captures are asked for by a tile being drawn and by nothing else, and
// the editor pumps them only behind the capture gate. Asserted on the source: the sweep needed a Vulkan
// device to observe running, and its absence has no runtime behaviour to observe.
TEST( ThumbnailPrefetch, NothingSweepsTheProjectForInvisibleAssets )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    EXPECT_FALSE( fs::exists( root + "Editor/Source/Editor/Widgets/ThumbnailSweep.hpp" ) )
         << "the project-wide thumbnail sweep is back: it queues captures for assets nobody is looking at";
    EXPECT_FALSE( fs::exists( root + "Editor/Source/Editor/Widgets/ThumbnailScan.cpp" ) );

    const std::string panel = ReadFile( root + "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp" );
    ASSERT_FALSE( panel.empty() );
    EXPECT_EQ( panel.find( "Sweep" ), std::string::npos ) << "the Content Browser drives a sweep again";
    EXPECT_NE( panel.find( "ThumbnailPrefetch::Get().Request(" ), std::string::npos )
         << "the Content Browser no longer hands its folder's cached pictures to the worker decode";

    const std::string layer = ReadFile( root + "Editor/Source/EditorLayer.cpp" );
    ASSERT_FALSE( layer.empty() );
    EXPECT_NE( layer.find( "if ( Splash::ThumbnailCaptureAllowed( CurrentRevealState() ) )\n"
                           "            ThumbnailService::Get().TickCapture();" ),
               std::string::npos )
         << "the capture half of the thumbnail pump is not behind the capture gate";
}

// The byte budget is asked as Background work: a UserSurface entitlement would let a queue of thumbnails
// eat the memory the next view a person opens needs (Desert/Tests/Engine/ViewBudget proves the rule itself).
TEST( ThumbnailPrefetch, TheServiceAsksTheViewBudgetAsBackgroundWork )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string code = ReadFile( root + "Editor/Source/Editor/Widgets/ThumbnailService.cpp" );
    ASSERT_FALSE( code.empty() );

    EXPECT_NE( code.find( "MayCreateView(" ), std::string::npos );
    EXPECT_NE( code.find( "Demand::Background" ), std::string::npos );
    EXPECT_EQ( code.find( "Demand::UserSurface" ), std::string::npos );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
