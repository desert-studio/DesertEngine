// "A cached thumbnail is decoded on a worker before it is drawn; nothing sweeps the project for captures."
//
// TH2 (owner: "as in UE"). ThumbnailPrefetch decodes PNGs that are already in the disk cache on a JobSystem
// worker — during the splash, for the folder the Content Browser opens on — so the first frame after the
// window is shown only uploads. Captures stay lazy: only a tile being drawn asks for one, and the
// project-wide background sweep is gone (decision В4).

#include <Common/Content/AssetEnvelope.hpp>

#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailOutdated.hpp>
#include <Editor/Widgets/ThumbnailPrefetch.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
        // A Windows checkout translates line endings and a binary read keeps the '\r'; the census below
        // searches for a statement that spans two lines.
        std::string text = ss.str();
        std::erase( text, '\r' );
        return text;
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
        // ofstream::write takes char bytes; these are the PNG bytes
        // NOLINTNEXTLINE(bugprone-casting-through-void)
        out.write( static_cast<const char*>( static_cast<const void*>( kOnePixelPng.data() ) ),
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
            std::ofstream( Source ) << R"({ "material": 1 })";
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
            // checked by the ASSERT_TRUE (fixture) above, which returns; tidy does not model gtest
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            ASSERT_TRUE( ThumbnailFreshness::Record( Png, hash.value() ) );
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
    // checked by the ASSERT_TRUE above, which returns; tidy does not model gtest
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const ThumbnailPixels& px = pixels.value();
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
    const std::string cache = ReadFile( std::format( "{}Editor/Source/Editor/Widgets/ThumbnailCache.cpp", root ) );
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
    // checked by the ASSERT_TRUE above, which returns; tidy does not model gtest
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    EXPECT_NE( second.Pixels.value().DecodedOn, std::this_thread::get_id() );
    EXPECT_EQ( prefetch.DecodedOnTheTakingThread() - before, 0u ) << "decoded on main thread: N > 0";
}

// THM1l-c6: a capture landing on screen is decoded ONCE. The pixels say when their read began, and a picture
// flagged rewritten is current once pixels read after the flag are cached — not after the stamp leaves the
// racy window (that rule re-queued the same file on every draw for a second: 25 decodes of one material).
TEST( ThumbnailPrefetch, PixelsSayWhenTheirReadBegan )
{
    const Fixture f;
    f.WriteFreshPng();
    const auto stamp    = fs::last_write_time( f.Png );
    auto&      prefetch = ThumbnailPrefetch::Get();

    const auto asked = std::chrono::steady_clock::now();
    EXPECT_FALSE( prefetch.Acquire( f.Png.string(), stamp ).Pixels.has_value() );
    prefetch.Drain();
    const auto taken = prefetch.Acquire( f.Png.string(), stamp );
    ASSERT_TRUE( taken.Pixels.has_value() );
    EXPECT_GE( taken.ReadBegan, asked ) << "a read queued after the ask cannot have begun before it";
    EXPECT_LE( taken.ReadBegan, std::chrono::steady_clock::now() );
}

TEST( ThumbnailOutdated, ARewriteIsSettledByTheFirstReadAfterItNotByTheClock )
{
    using Clock = ThumbnailOutdated::Clock;
    ThumbnailOutdated outdated;
    const std::string png  = "a.png";
    const auto        seen = Clock::now();

    EXPECT_TRUE( outdated.Settle( png, seen ) ) << "a path never flagged is current";
    outdated.Flag( png, seen );
    EXPECT_TRUE( outdated.Contains( png ) );

    // Pixels a worker began reading before the rewrite was seen are the old file: still outdated, asked again.
    EXPECT_FALSE( outdated.Settle( png, seen - std::chrono::milliseconds( 1 ) ) );
    EXPECT_TRUE( outdated.Contains( png ) );

    // The first read at or after the flag settles it — once; the next Get serves the cache, no decode.
    EXPECT_TRUE( outdated.Settle( png, seen ) );
    EXPECT_FALSE( outdated.Contains( png ) );

    // A second rewrite flags it anew, and the later moment is the one a read must follow.
    outdated.Flag( png, seen + std::chrono::milliseconds( 5 ) );
    EXPECT_FALSE( outdated.Settle( png, seen + std::chrono::milliseconds( 1 ) ) );
    outdated.Forget( png );
    EXPECT_FALSE( outdated.Contains( png ) );
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

    EXPECT_FALSE( fs::exists( std::format( "{}Editor/Source/Editor/Widgets/ThumbnailSweep.hpp", root ) ) )
         << "the project-wide thumbnail sweep is back: it queues captures for assets nobody is looking at";
    EXPECT_FALSE( fs::exists( std::format( "{}Editor/Source/Editor/Widgets/ThumbnailScan.cpp", root ) ) );

    const std::string panel =
         ReadFile( std::format( "{}Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp", root ) );
    ASSERT_FALSE( panel.empty() );
    EXPECT_EQ( panel.find( "Sweep" ), std::string::npos ) << "the Content Browser drives a sweep again";
    EXPECT_NE( panel.find( "ThumbnailPrefetch::Get().Request(" ), std::string::npos )
         << "the Content Browser no longer hands its folder's cached pictures to the worker decode";

    const std::string layer = ReadFile( std::format( "{}Editor/Source/EditorLayer.cpp", root ) );
    ASSERT_FALSE( layer.empty() );
    EXPECT_NE(
         layer.find(
              "if ( Splash::ThumbnailCaptureAllowed( CurrentRevealState() ) )\n"
              "            ThumbnailService::Get().TickCapture( ThumbnailWarmup::CaptureScope::Everything );" ),
         std::string::npos )
         << "the capture half of the thumbnail pump is not behind the capture gate";
    // THUMB3: the only capture before the hand-over is the scene's, behind its own gate and scope.
    EXPECT_NE(
         layer.find(
              "else if ( Splash::SceneThumbnailCaptureAllowed( CurrentRevealState() ) &&\n"
              "                  ThumbnailService::Get().SceneWarmPending() > 0 )\n"
              "            ThumbnailService::Get().TickCapture( ThumbnailWarmup::CaptureScope::SceneWarmOnly );" ),
         std::string::npos )
         << "a capture on the splash is not limited to the scene's warm list";
}

// THUMB2. The splash uploads the opening folder's pictures before the hand-over, and it reads where they stand
// through SurveyOf: a picture still with a worker holds the hand-over (within its budget), a finished one is
// uploaded, and a MISSING one is neither — it is a capture, and a capture waits for the window.
TEST( ThumbnailPrefetch, TheSplashSurveySeesWhatIsReadyAndNeverWaitsForACapture )
{
    const Fixture f;
    f.WriteFreshPng();
    const fs::path missingSource = f.Dir / "M_NoPicture.demat";
    const fs::path missingPng    = f.Dir / "M_NoPicture.png";
    std::ofstream( missingSource ) << R"({ "material": 2 })";

    const std::vector<ThumbnailPrefetch::Item> folder = { { f.Png.string(), f.Source.string() },
                                                          { missingPng.string(), missingSource.string() } };
    ThumbnailPrefetch::Get().Request( folder );

    const ThumbnailPrefetch::Survey before = ThumbnailPrefetch::Get().SurveyOf( folder );
    EXPECT_TRUE( before.Ready.empty() );
    EXPECT_EQ( before.Pending, 2u ) << "the requested pictures must hold the hand-over while a worker has them";

    ThumbnailPrefetch::Get().Drain();
    const ThumbnailPrefetch::Survey after = ThumbnailPrefetch::Get().SurveyOf( folder );
    ASSERT_EQ( after.Ready.size(), 1u ) << "the decoded cached picture is not offered to the splash upload";
    EXPECT_EQ( after.Ready.front(), f.Png.string() );
    EXPECT_EQ( after.Pending, 0u ) << "a picture with no PNG on disk held the splash: that is a capture's wait";
    EXPECT_FALSE( fs::exists( missingPng ) )
         << "the splash pass produced a thumbnail: captures wait for the window";

    // Uploaded (the cache took it): it neither holds the hand-over nor is offered again.
    ASSERT_TRUE( ThumbnailPrefetch::Get().Take( f.Png.string(), fs::last_write_time( f.Png ) ).has_value() );
    const ThumbnailPrefetch::Survey uploaded = ThumbnailPrefetch::Get().SurveyOf( folder );
    EXPECT_TRUE( uploaded.Ready.empty() );
    EXPECT_EQ( uploaded.Pending, 0u ) << "an uploaded picture kept holding the hand-over to its budget";
}

// The wiring the suite cannot link (EditorLayer, the panel): the panel's constructor prefetches the folder it
// opens on, the splash pass uploads exactly that list, the stages tick the decode, and the capture stays gated.
TEST( ThumbnailPrefetch, TheSplashUploadsTheFolderTheBrowserOpensOn )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string panel =
         ReadFile( std::format( "{}Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp", root ) );
    ASSERT_FALSE( panel.empty() );
    EXPECT_NE( panel.find( "m_PrefetchItems = items;\n" ), std::string::npos )
         << "the splash upload no longer reads the list the browser prefetched";
    // THM1n-13: the folder's list AND the project's — every picture of the project is uploaded on the splash.
    EXPECT_NE(
         panel.find(
              "items.insert( items.end(), m_ProjectPrefetchItems.begin(), m_ProjectPrefetchItems.end() );\n"
              "        const ThumbnailPrefetch::Survey survey = ThumbnailPrefetch::Get().SurveyOf( items );" ),
         std::string::npos )
         << "the splash upload no longer reads the project's pictures with the folder's";
    EXPECT_NE( panel.find( "(void)m_Thumbnails->Get( picture );" ), std::string::npos )
         << "the splash upload does not go through the cache the tiles draw from";

    const std::string layer = ReadFile( std::format( "{}Editor/Source/EditorLayer.cpp", root ) );
    ASSERT_FALSE( layer.empty() );
    EXPECT_NE(
         layer.find( "            ThumbnailService::TickDiskAndDecode();\n        UploadSplashThumbnails();\n" ),
         std::string::npos )
         << "the per-frame thumbnail pump no longer runs the splash upload pass";
    EXPECT_NE( layer.find( "m_FileExplorerPanel->UploadPrefetchedThumbnails()" ), std::string::npos );
    EXPECT_NE(
         layer.find(
              "                ThumbnailService::TickDiskAndDecode();\n            SampleFrameQuiescence();\n"
              "            return BOOLSUCCESS;" ),
         std::string::npos )
         << "the startup stages no longer tick the worker decode";
    EXPECT_NE( layer.find( "state.ThumbnailsUploading = m_ThumbnailsHoldReveal;" ), std::string::npos );
}

// The byte budget is asked as Background work: a UserSurface entitlement would let a queue of thumbnails
// eat the memory the next view a person opens needs (Desert/Tests/Engine/ViewBudget proves the rule itself).
TEST( ThumbnailPrefetch, TheServiceAsksTheViewBudgetAsBackgroundWork )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string code =
         ReadFile( std::format( "{}Editor/Source/Editor/Widgets/ThumbnailService.cpp", root ) );
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

namespace
{
    // A `.detex` as the importer writes one: an envelope stating the Texture kind, with (or without) the
    // imported file's bytes as its SRCE section.
    fs::path WriteTextureAsset( const fs::path& dir, bool withSource )
    {
        namespace CC = Common::Content;
        CC::AssetEnvelope envelope;
        envelope.Asset.Kind = CC::ContentKind::Texture;
        envelope.Asset.Guid = CC::AssetGuid::Generate();
        if ( withSource )
        {
            CC::EnvelopeSectionData source;
            source.Tag = CC::EnvelopeSection::Source;
            for ( const unsigned char byte : kOnePixelPng )
                source.Bytes.push_back( static_cast<std::byte>( byte ) );
            envelope.Sections.push_back( std::move( source ) );
        }
        const fs::path file    = dir / ( withSource ? "T_Source.detex" : "T_Cooked.detex" );
        const auto     written = CC::WriteAssetEnvelopeFile( file, envelope );
        EXPECT_TRUE( written.IsSuccess() ) << ( written.IsSuccess() ? "" : written.GetError() );
        return file;
    }
} // namespace

// THM1n-3. The imported texture asset is its source, wrapped: its picture is the SRCE section decoded, the
// same pixels the loose .png would give. Before this the Textures folder showed a grey glyph per `.detex`.
TEST( ThumbnailPrefetch, ATextureAssetIsDecodedFromTheSourceItCarries )
{
    const fs::path dir = fs::temp_directory_path() / "desert_thumbnail_detex";
    fs::remove_all( dir );
    fs::create_directories( dir );

    const auto decoded = ThumbnailPixels::Decode( WriteTextureAsset( dir, true ).string() );
    if ( !decoded.has_value() )
        FAIL() << "a .detex with a source section produced no picture";
    EXPECT_EQ( decoded->SourceWidth, 1 );
    EXPECT_EQ( decoded->SourceHeight, 1 );
    EXPECT_EQ( decoded->Rgba.size(), 4U );

    // No source (a cooked texture keeps only its derived-data key): no picture, said by name — never a
    // blank square passed off as the texture.
    EXPECT_FALSE( ThumbnailPixels::Decode( WriteTextureAsset( dir, false ).string() ).has_value() );
    fs::remove_all( dir );
}

// THM1n-13: the splash made every picture of the project resident, so entering a folder after the hand-over
// hands the workers NOTHING — Unresident drops what the browser's ThumbnailCache already holds (Holds), and the
// panel requests only what is left. Zero decodes on a folder change; a picture not yet held is still decoded.
TEST( ThumbnailPrefetch, AFolderOfResidentPicturesDecodesNothingWhenEntered )
{
    const Fixture f;
    f.WriteFreshPng();
    const std::vector<ThumbnailPrefetch::Item> folder = { { f.Png.string(), f.Source.string() } };

    // The splash: decoded and taken (uploaded) — the cache now holds it.
    ThumbnailPrefetch::Get().Request( folder );
    ThumbnailPrefetch::Get().Drain();
    ASSERT_TRUE( ThumbnailPrefetch::Get().Take( f.Png.string(), fs::last_write_time( f.Png ) ).has_value() );
    const auto resident = [&]( const std::string& picture ) { return picture == f.Png.string(); };

    // The folder is entered: nothing to decode.
    ThumbnailPrefetch::Get().Request( ThumbnailPrefetch::Unresident( folder, resident ) );
    EXPECT_FALSE( ThumbnailPrefetch::Get().Pending( f.Png.string() ) ) << "a resident picture was queued again";
    ThumbnailPrefetch::Get().Drain();
    EXPECT_EQ( ThumbnailPrefetch::Get().ReadyCount(), 0u ) << "entering a folder decoded a resident picture";

    // Nothing resident yet: the same folder is decoded.
    EXPECT_EQ( ThumbnailPrefetch::Unresident( folder, []( const std::string& ) { return false; } ).size(), 1u );

    // The panel filters every request it makes through the cache, and the cache has no size cap that would drop
    // a resident picture (read from the source: both need a device).
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string panel =
         ReadFile( std::format( "{}Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp", root ) );
    std::size_t requests = 0;
    std::size_t filtered = 0;
    for ( std::size_t at = panel.find( "ThumbnailPrefetch::Get().Request(" ); at != std::string::npos;
          at             = panel.find( "ThumbnailPrefetch::Get().Request(", at + 1 ) )
    {
        ++requests;
        if ( panel.compare( at, 64, "ThumbnailPrefetch::Get().Request( ThumbnailPrefetch::Unresident(" ) == 0 )
            ++filtered;
    }
    EXPECT_GT( requests, 0u );
    EXPECT_EQ( requests, filtered ) << "the browser hands a resident picture to the workers again";
    const std::string cache = ReadFile( std::format( "{}Editor/Source/Editor/Widgets/ThumbnailCache.cpp", root ) );
    EXPECT_EQ( cache.find( "kMaxEntries" ), std::string::npos ) << "the thumbnail cache is capped again";
    EXPECT_EQ( panel.find( "m_Thumbnails->Clear()" ), std::string::npos )
         << "the browser wipes its resident pictures again (a rescan)";
}
