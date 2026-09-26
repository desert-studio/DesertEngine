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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace Desert::Editor;
namespace fs = std::filesystem;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Editor/Source/Editor/Widgets/ThumbnailService.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
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

    void WritePng( const fs::path& path )
    {
        std::ofstream out( path, std::ios::binary );
        out.write( static_cast<const char*>( static_cast<const void*>( kOnePixelPng.data() ) ),  // NOLINT(bugprone-casting-through-void): ofstream::write takes char bytes; these are the PNG bytes
                   static_cast<std::streamsize>( kOnePixelPng.size() ) );
    }

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
            WritePng( Png );
            const auto hash = ThumbnailFreshness::ContentHash( Source );
            ASSERT_TRUE( hash.has_value() );
            ASSERT_TRUE( ThumbnailFreshness::Record( Png, hash.value() ) );  // NOLINT(bugprone-unchecked-optional-access): checked by the ASSERT_TRUE (fixture) above, which returns; tidy does not model gtest
        }
    };
} // namespace

TEST( ThumbnailPrefetch, ACachedPngIsDecodedOnAWorkerBeforeAnyoneDrawsIt )
{
    const Fixture f;
    f.WriteFreshPng();

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), f.Source.string() } } );
    ThumbnailPrefetch::Get().Drain();
    ASSERT_EQ( ThumbnailPrefetch::Get().ReadyCount(), 1u );

    const auto pixels = ThumbnailPrefetch::Get().Take( f.Png.string(), fs::last_write_time( f.Png ) );
    ASSERT_TRUE( pixels.has_value() ) << "the cached PNG was not decoded ahead of the draw";
    const ThumbnailPixels& px = pixels.value();  // NOLINT(bugprone-unchecked-optional-access): checked by the ASSERT_TRUE above, which returns; tidy does not model gtest
    EXPECT_EQ( px.Width, 1 );
    EXPECT_EQ( px.Height, 1 );
    EXPECT_EQ( px.Rgba.size(), 4u );
    EXPECT_NE( px.DecodedOn, std::this_thread::get_id() )
         << "the prefetch decoded on the thread that asked for it: that is the main thread in the editor";
}

// The browser asks for its folder and draws it in the SAME frame. If Get() decoded a picture a worker already
// holds, the main thread would pay for it anyway (the 52 ms gif at startup): a pending picture is announced,
// and the cache waits for it instead of decoding it again.
TEST( ThumbnailPrefetch, APictureAWorkerHoldsIsNotDecodedAgainByTheDraw )
{
    const Fixture f;
    WritePng( f.Png );

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), {} } } );
    EXPECT_TRUE( ThumbnailPrefetch::Get().Pending( f.Png.string() ) ) << "a requested picture is not announced";
    ThumbnailPrefetch::Get().Tick();
    EXPECT_TRUE( ThumbnailPrefetch::Get().Pending( f.Png.string() ) ||
                 ThumbnailPrefetch::Get().ReadyCount() == 1u );
    ThumbnailPrefetch::Get().Drain();
    EXPECT_FALSE( ThumbnailPrefetch::Get().Pending( f.Png.string() ) );
    EXPECT_FALSE( ThumbnailPrefetch::Get().Pending( ( f.Dir / "never_requested.png" ).string() ) );

    // ThumbnailCache::Get needs a device, so its half of the contract is read from the source: it takes pixels
    // through Acquire (which never decodes, see the next test) and has no decoder of its own to fall back on.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string cache = ReadFile( root + "Editor/Source/Editor/Widgets/ThumbnailCache.cpp" );
    ASSERT_FALSE( cache.empty() );
    EXPECT_NE( cache.find( "ThumbnailPrefetch::Get().Acquire( sourcePath, stamp )" ), std::string::npos )
         << "Get() no longer takes its pixels through the prefetch";
    EXPECT_EQ( cache.find( "ThumbnailPixels::Decode(" ), std::string::npos )
         << "Get() decodes on the main thread again";
}

// Materials, measured: ~1.5 s after the folder opened, 36-42 pictures were decoded on the main thread at ~24 ms
// each — pictures a capture had just rewritten, and pictures a rescan dropped from the cache — none of which
// the folder's prefetch still held. The draw asks for such a picture and gets NOTHING on that frame: the file
// is queued for a worker, and a later frame takes pixels the worker decoded from the file as it is now.
TEST( ThumbnailPrefetch, APictureACaptureRewroteGoesThroughAWorkerNotTheDraw )
{
    const Fixture f;
    f.WriteFreshPng();
    auto& prefetch = ThumbnailPrefetch::Get();

    // The folder's prefetch decoded the old picture...
    prefetch.Request( { { f.Png.string(), f.Source.string() } } );
    prefetch.Drain();

    // ...then a capture rewrote it (a newer stamp than the decode carries).
    WritePng( f.Png );
    const auto newer = fs::last_write_time( f.Png ) + std::chrono::seconds( 5 );
    fs::last_write_time( f.Png, newer );

    const std::size_t before = prefetch.DecodedOnTheTakingThread();
    const auto        first  = prefetch.Acquire( f.Png.string(), newer );
    EXPECT_FALSE( first.Pixels.has_value() ) << "stale pixels were handed over as the rewritten picture";
    EXPECT_FALSE( first.Undecodable );
    EXPECT_TRUE( prefetch.Pending( f.Png.string() ) ) << "the rewritten picture was not queued for a worker";

    // A second draw before the worker is done queues nothing twice and decodes nothing.
    EXPECT_FALSE( prefetch.Acquire( f.Png.string(), newer ).Pixels.has_value() );

    prefetch.Drain();
    auto second = prefetch.Acquire( f.Png.string(), newer );
    ASSERT_TRUE( second.Pixels.has_value() ) << "the worker never delivered the rewritten picture";
    EXPECT_NE( second.Pixels.value().DecodedOn, std::this_thread::get_id() );  // NOLINT(bugprone-unchecked-optional-access): checked by the ASSERT_TRUE above, which returns; tidy does not model gtest
    EXPECT_EQ( prefetch.DecodedOnTheTakingThread() - before, 0u ) << "decoded on main thread: N > 0";
}

// A picture a worker could not decode is reported as such (so a generated one is deleted and re-captured),
// not re-queued every frame forever.
TEST( ThumbnailPrefetch, AnUndecodablePictureIsReportedNotRequeued )
{
    const Fixture f;
    std::ofstream( f.Png ) << "not a png";
    const auto stamp = fs::last_write_time( f.Png );

    auto& prefetch = ThumbnailPrefetch::Get();
    EXPECT_FALSE( prefetch.Acquire( f.Png.string(), stamp ).Undecodable );
    prefetch.Drain();
    const auto result = prefetch.Acquire( f.Png.string(), stamp );
    EXPECT_FALSE( result.Pixels.has_value() );
    EXPECT_TRUE( result.Undecodable );
    EXPECT_FALSE( prefetch.Pending( f.Png.string() ) );
}

TEST( ThumbnailPrefetch, NoPngOnDiskMeansNothingIsDecodedAndNothingIsCaptured )
{
    const Fixture f; // no PNG: the capture is owed, and the capture queue waits for the window

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), f.Source.string() } } );
    ThumbnailPrefetch::Get().Drain();
    EXPECT_FALSE( ThumbnailPrefetch::Get().Take( f.Png.string(), {} ).has_value() );
}

TEST( ThumbnailPrefetch, AFileRewrittenSinceTheDecodeIsNotHandedOver )
{
    const Fixture f;
    f.WriteFreshPng();

    ThumbnailPrefetch::Get().Request( { { f.Png.string(), f.Source.string() } } );
    ThumbnailPrefetch::Get().Drain();
    const auto later = fs::last_write_time( f.Png ) + std::chrono::seconds( 5 );
    EXPECT_FALSE( ThumbnailPrefetch::Get().Take( f.Png.string(), later ).has_value() )
         << "pixels of an older file would be uploaded as the picture of the new one";
}

TEST( ThumbnailPrefetch, AUsersOwnImageNeedsNoFreshnessRecord )
{
    const Fixture f;
    WritePng( f.Png );

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
