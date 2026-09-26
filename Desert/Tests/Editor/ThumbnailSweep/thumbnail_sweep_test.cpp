// "A THUMBNAIL THAT NOBODY ASKED FOR STILL ARRIVES — AND IT NEVER TAKES THE MACHINE WITH IT."
//
// WHAT THIS SUITE IS ABOUT. Before M11 a rendered thumbnail existed only if a panel had drawn its tile,
// so a project opened on a cold cache was a grid of grey squares until somebody scrolled through every
// folder, and a file dropped into a directory nobody had open never acquired a picture at all. The sweep
// (Editor/Widgets/ThumbnailSweep.hpp) is what makes them arrive on their own. Two things about it have to
// be true and neither is visible in a frame:
//
//   * IT MUST FIND THE RIGHT FILES — and only those. A sweep that queued textures would spend a renderer
//     on a picture the file already is; one that queued scenes would invent a camera angle nobody chose;
//     one that re-queued fresh assets would re-render the whole content tree every three seconds for ever.
//   * IT MUST NOT TAKE THE MACHINE. There are six renderer slots and a preview must be DESTROYED to give
//     one back, so background work that grabbed them in a batch would close the surfaces a person is
//     actually looking at. The lead's instruction was explicit: the sweep's budget is to be ASSERTED BY A
//     TEST rather than observed by attention.
//
// WHY IT CAN BE A UNIT TEST AT ALL. `ThumbnailScan.cpp` was deliberately split out of `ThumbnailSweep.cpp`
// so that the deciding half — which files need a picture, and how many one frame may hand over — includes
// nothing that reaches a Vulkan device. That file is the only editor source this suite compiles.

#include <Editor/Widgets/ThumbnailFormats.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailSweep.hpp>

#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace Desert::Editor;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Widgets/ThumbnailSweep.hpp" );
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

    void WriteFile( const fs::path& path, const std::string& contents )
    {
        fs::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary );
        out << contents;
    }

    /// A temporary project, torn down with the globals restored.
    ///
    /// THE GLOBALS MATTER HERE and are the reason this is a fixture rather than a local directory:
    /// `ThumbnailKey::DiskPath` composes its answer under `Constants::Path::COOKED_PATH`, and
    /// `StableKeyForPath` resolves an asset's identity against the content roots. A test that left those
    /// pointing at the sandbox would write its PNGs somewhere else entirely and then wonder why the sweep
    /// still called every asset stale.
    class TempProject
    {
    public:
        TempProject()
        {
            m_Root = fs::temp_directory_path() /
                     ( "desert_m11_sweep_" +
                       std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
            fs::create_directories( m_Root / "Assets" );
            Common::Constants::Path::SetProjectRoot( m_Root, "Assets" );
        }

        ~TempProject()
        {
            Common::Constants::Path::ResetToSandbox();
            std::error_code ec;
            fs::remove_all( m_Root, ec );
        }

        [[nodiscard]] fs::path Assets() const
        {
            return m_Root / "Assets";
        }

    private:
        fs::path m_Root;
    };

    bool Mentions( const std::vector<ThumbnailSweepCandidate>& candidates, const std::string& needle )
    {
        return std::any_of( candidates.begin(), candidates.end(), [&needle]( const ThumbnailSweepCandidate& c )
                            { return c.AssetPath.find( needle ) != std::string::npos; } );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE SWEEP FINDS WHAT HAS NO PICTURE — and nothing else.
//
// One directory holding one file of every ANSWER the census can give, so the test fails if any of the
// five producer kinds is treated as another. This is the shape that catches the two mistakes with the
// worst consequences: sweeping a texture (a renderer spent on a file that IS the picture) and sweeping a
// scene (the engine choosing a camera angle that belongs to whoever authored the level).
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, OnlyGeneratedProducersAreSwept )
{
    TempProject project;

    WriteFile( project.Assets() / "Materials/Oak.demat", "{}" );             // RenderedMaterial
    WriteFile( project.Assets() / "Clouds/Types/A.decloudtype", "{}" );      // Painted
    WriteFile( project.Assets() / "Textures/Bark.png", "not really a png" ); // Decoded
    WriteFile( project.Assets() / "Scenes/Level.desce", "{}" );              // Authored
    WriteFile( project.Assets() / "Scripts/Player.lua", "-- hi" );           // None
    WriteFile( project.Assets() / "Notes/readme.txt", "hello" );             // not a format at all

    const auto found = ScanForMissingThumbnails( project.Assets(), 64 );

    EXPECT_TRUE( Mentions( found, "Oak.demat" ) )
         << "a material with no cached picture was not swept. This is the case the whole feature exists "
            "for: a cold cache must fill itself with nobody pressing anything.";
    EXPECT_TRUE( Mentions( found, "A.decloudtype" ) )
         << "a cloud type with no cached picture was not swept. The four cloud formats are the reason "
            "M11 had to widen the producer set at all — the owner could not pick a cloud by looking.";

    EXPECT_FALSE( Mentions( found, "Bark.png" ) )
         << "an image was swept. A texture IS its own picture (Producer::Decoded); generating a second "
            "one would spend a capture to arrive at the file we started with.";
    EXPECT_FALSE( Mentions( found, "Level.desce" ) )
         << "a scene was swept. A level's picture is a SHOT of it — where the camera stands is a decision "
            "belonging to whoever authored the level, which is why 'Capture Thumbnail (from viewport)' is "
            "the only producer for it (Producer::Authored). Generating one would also mean loading every "
            "level in the project to photograph it.";
    EXPECT_FALSE( Mentions( found, "Player.lua" ) )
         << "a script was swept. Producer::None rows carry a written reason there must be no picture, and "
            "sweeping one would queue work whose result nothing would ever draw.";
    EXPECT_FALSE( Mentions( found, "readme.txt" ) ) << "a file the Content Browser does not even type was swept.";
}

// ---------------------------------------------------------------------------------------------------
// 2. A FRESH PICTURE IS NOT RE-MADE, AND A STALE ONE IS.
//
// This is trigger 3 of the owner's three, and it is the one that costs the most when it is wrong in
// either direction: too eager and the editor re-renders the whole content tree every few seconds for
// ever; too lax and an asset edited outside the editor keeps a picture of what it used to be.
//
// It is asked through ThumbnailFreshness — the same rule the browser uses to decide whether to DRAW the
// file — deliberately, because those two questions disagreeing is exactly the defect M8 paid for.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, AFreshPictureIsLeftAloneAndAStaleOneIsSweptAgain )
{
    TempProject project;

    const fs::path asset = project.Assets() / "Materials/Fresh.demat";
    WriteFile( asset, "{}" );
    const std::string png = ThumbnailKey::DiskPath( asset.generic_string() );
    WriteFile( png, "PNG" );
    ASSERT_TRUE(
         ThumbnailFreshness::Record( png, ThumbnailFreshness::ContentHash( asset ).value_or( 0 ) ).IsSuccess() );

    EXPECT_FALSE( Mentions( ScanForMissingThumbnails( project.Assets(), 64 ), "Fresh.demat" ) )
         << "a material whose picture is already on disk and newer than it was queued anyway. A sweep "
            "that cannot tell 'done' from 'to do' re-renders the entire project every scan.";

    // A modtime move alone is not an edit (TH1): the picture still shows the same bytes.
    fs::last_write_time( asset, fs::last_write_time( asset ) + std::chrono::seconds( 30 ) );
    EXPECT_FALSE( Mentions( ScanForMissingThumbnails( project.Assets(), 64 ), "Fresh.demat" ) )
         << "a touched but unchanged material was queued: every launch after a checkout re-renders it.";

    // Edit the source: the picture is now of something else.
    WriteFile( asset, R"({ "Roughness": 0.9 })" );

    EXPECT_TRUE( Mentions( ScanForMissingThumbnails( project.Assets(), 64 ), "Fresh.demat" ) )
         << "a material edited after its picture was written was NOT queued. Every reader already refuses "
            "to draw that PNG; if the sweep also refuses to replace it, the asset shows a placeholder for "
            "the rest of the project's life — which is M8's defect, arriving again through a new door.";
}

// ---------------------------------------------------------------------------------------------------
// 3. A MESH IS FILED UNDER THE FILE THAT IS PHOTOGRAPHED.
//
// The one format whose picture is of a DIFFERENT file. Getting this wrong is not hypothetical: the
// browser filed a mesh's thumbnail under its source while the Details row filed it under the cooked form,
// so one mesh was captured twice, at 370 ms and ~200 KB a time, and neither copy could satisfy the other
// panel. The sweep is now a third asker of that question and must give the same answer as the other two.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, AMeshIsSweptUnderItsCookedFormAndNotItsSource )
{
    TempProject project;
    WriteFile( project.Assets() / "Meshes/Rock.fbx", "not really an fbx" );

    const auto found = ScanForMissingThumbnails( project.Assets(), 64 );
    ASSERT_TRUE( Mentions( found, "Rock.fbx" ) ) << "a mesh source was not swept at all";

    for ( const ThumbnailSweepCandidate& candidate : found )
    {
        if ( candidate.AssetPath.find( "Rock.fbx" ) == std::string::npos )
            continue;

        EXPECT_NE( candidate.Subject.find( ".stmesh" ), std::string::npos )
             << "the mesh's SUBJECT is '" << candidate.Subject
             << "', not its cooked .stmesh. A StaticMeshAsset loads cooked JSON and never opens the FBX, "
                "so the cook is this picture's recipe and the only file whose modification time means "
                "anything about it.";
        EXPECT_EQ( candidate.Png, ThumbnailKey::DiskPath( candidate.Subject ) )
             << "the mesh's picture is filed under a key that is not its subject's. Two keys for one "
                "picture is how the same mesh came to be photographed twice under two names.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 4. THE BUDGET. One frame hands over at most kRequestsPerFrame, and everything eventually goes.
//
// BOTH HALVES MATTER AND THEY FAIL IN OPPOSITE DIRECTIONS. Without the cap, a cold cache of a thousand
// assets is a thousand AssetManager lookups and cooked-JSON parses in ONE frame — a visible stall caused
// by work nobody asked for. Without the completeness half, a cap is indistinguishable from a queue that
// drops everything past the first eight, which is a feature that looks like it works on a small project.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, OneFrameHandsOverAtMostItsShareAndTheWholeBatchEventuallyGoes )
{
    constexpr int kTotal = 100;

    std::vector<ThumbnailSweepCandidate> pending;
    for ( int i = 0; i < kTotal; ++i )
    {
        ThumbnailSweepCandidate candidate;
        candidate.AssetPath = "Materials/M" + std::to_string( i ) + ".demat";
        candidate.Subject   = candidate.AssetPath;
        candidate.Png       = candidate.AssetPath + ".png";
        candidate.By        = ThumbnailFormats::Producer::RenderedMaterial;
        pending.push_back( candidate );
    }

    ThumbnailSweeper sweeper;
    sweeper.SetPendingForTest( pending );

    std::vector<std::string> handed;
    int                      frames = 0;
    while ( sweeper.PendingCount() > 0 )
    {
        const int took = sweeper.Drain( [&handed]( const ThumbnailSweepCandidate& candidate )
                                        { handed.push_back( candidate.AssetPath ); } );
        ASSERT_LE( took, ThumbnailSweeper::kRequestsPerFrame )
             << "one frame handed over " << took << " requests, past the budget of "
             << ThumbnailSweeper::kRequestsPerFrame
             << ". Each one can cost a cooked-mesh parse; a frame is not the place to do a thousand.";
        ASSERT_GT( took, 0 ) << "the drain stalled with " << sweeper.PendingCount()
                             << " still pending — a sweep that stops draining looks exactly like a sweep "
                                "with nothing to do";
        ++frames;
        ASSERT_LT( frames, kTotal + 2 ) << "the drain is not making progress";
    }

    EXPECT_EQ( static_cast<int>( handed.size() ), kTotal )
         << "the batch was capped rather than paced: " << handed.size() << " of " << kTotal
         << " ever reached the service. A cap that drops the remainder is a feature that works only on "
            "projects small enough not to need it.";

    const std::set<std::string> unique( handed.begin(), handed.end() );
    EXPECT_EQ( unique.size(), handed.size() ) << "an asset was handed over twice in one pass";

    EXPECT_EQ( frames, ( kTotal + ThumbnailSweeper::kRequestsPerFrame - 1 ) / ThumbnailSweeper::kRequestsPerFrame )
         << "the pass took " << frames << " frames for " << kTotal << " assets at "
         << ThumbnailSweeper::kRequestsPerFrame << " a frame — the pacing is not what the constant says";
}

// ---------------------------------------------------------------------------------------------------
// 4b. THE SCAN IS BOUNDED TOO.
//
// The per-frame cap paces the HAND-OVER; this caps what one pass may find. Without it, a cold cache on a
// large project hands the main thread a list it takes hours to walk at eight a frame — and a file
// dropped in during those hours sits behind all of it, because the next scan cannot start until this
// batch drains.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, OneScanReportsAtMostItsLimit )
{
    TempProject project;
    for ( int i = 0; i < 40; ++i )
        WriteFile( project.Assets() / ( "Materials/M" + std::to_string( i ) + ".demat" ), "{}" );

    const auto found = ScanForMissingThumbnails( project.Assets(), 7 );
    EXPECT_EQ( found.size(), 7u ) << "a scan asked for 7 candidates returned " << found.size();

    EXPECT_TRUE( ScanForMissingThumbnails( project.Assets(), 0 ).empty() )
         << "a limit of zero must mean zero, not 'unbounded' — the two readings differ by the whole "
            "content tree";
}

// ---------------------------------------------------------------------------------------------------
// 5. THE SWEEP CANNOT TAKE A RENDERER SLOT.
//
// The trap the lead named: there are six slots (EngineContext::kMaxRendererSlots) and a preview must be
// DESTROYED to give one back, so background work that claimed them in a batch would close the windows a
// person is looking at.
//
// The guarantee is STRUCTURAL rather than numeric, so it is asserted structurally: the sweep never builds
// a renderer, it only appends to ThumbnailService's queues, and that service holds exactly one renderer
// behind ViewBudget. Asserted by reading the sources because the alternative — reaching a
// sustained six-of-six — needs several scene views open at once, which no test can arrange (the same
// argument the ViewBudget suite makes for itself).
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, TheSweepBuildsNoRendererOfItsOwn )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    for ( const char* file :
          { "Editor/Source/Editor/Widgets/ThumbnailSweep.cpp", "Editor/Source/Editor/Widgets/ThumbnailScan.cpp" } )
    {
        const std::string code = ReadFile( root + file );
        ASSERT_FALSE( code.empty() ) << "could not read " << file;

        EXPECT_EQ( code.find( "AssetThumbnailRenderer(" ), std::string::npos )
             << file
             << " constructs an AssetThumbnailRenderer. Each one owns a full SceneRenderer and therefore "
                "one of the six renderer slots; a background pass that built its own would starve the "
                "surfaces the person opened — and would be taking the slot in order to produce the "
                "picture they see BECAUSE they could not have a live preview.";
        EXPECT_EQ( code.find( "SceneRenderer" ), std::string::npos )
             << file << " names SceneRenderer. The sweep queues; it does not render.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 5b. AND THE PAINTED FORMATS COST NOTHING AT ALL.
//
// The arithmetic the sweep's header states, asserted rather than described: a sweep at full tilt leaves
// five of the six slots for surfaces a person opens by hand, and a project made of cloud assets leaves
// all six, because those pictures are computed on a worker.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, PaintedPicturesClaimNoSlotAndRenderedOnesClaimAtMostOne )
{
    EXPECT_FALSE( ThumbnailFormats::NeedsRendererSlot( ThumbnailFormats::Producer::Painted ) )
         << "a painted picture is claimed to need a renderer slot. It is a decode and a fill on a "
            "JobSystem worker; saying otherwise would make a project of clouds wait behind "
            "ViewBudget for a resource it never touches.";
    EXPECT_TRUE( ThumbnailFormats::NeedsRendererSlot( ThumbnailFormats::Producer::RenderedMaterial ) );
    EXPECT_TRUE( ThumbnailFormats::NeedsRendererSlot( ThumbnailFormats::Producer::RenderedMesh ) );

    for ( const ThumbnailFormats::Producer by :
          { ThumbnailFormats::Producer::Decoded, ThumbnailFormats::Producer::Authored,
            ThumbnailFormats::Producer::None } )
    {
        EXPECT_FALSE( ThumbnailFormats::IsGenerated( by ) )
             << "a producer the sweep must never schedule is marked as generated";
        EXPECT_FALSE( ThumbnailFormats::NeedsRendererSlot( by ) );
    }
}

// ---------------------------------------------------------------------------------------------------
// 6. THERE IS ONE IMPLEMENTATION OF "WHAT FILES ARE UNDER THIS ROOT".
//
// The lead's standing objection to this task, in his own framing: after M11 the tree must not hold TWO
// directory walks with nobody having written down how they differ. It holds two ASKERS —
// AssetPreloader ("which assets exist so a handle can resolve?") and this sweep ("which files the
// browser can show have no picture?") — and they are different questions over overlapping directories.
// What they must NOT be is two enumerations: `ListFilesRecursive` is the only one that also sees a
// mounted .dpak, and the font and icon services each hand-rolled the disk half once, so a packaged game
// scanned nothing and no text could resolve its font.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, NeitherContentWalkerRollsItsOwnDirectoryIteration )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const char* file : { "Editor/Source/Editor/Widgets/ThumbnailScan.cpp",
                               "Desert/Desert/Source/Engine/Assets/AssetPreloader.cpp" } )
    {
        const std::string code = ReadFile( root + file );
        ASSERT_FALSE( code.empty() ) << "could not read " << file;

        EXPECT_NE( code.find( "ListFilesRecursive" ), std::string::npos )
             << file
             << " no longer enumerates content through Common::Utils::FileSystem::ListFilesRecursive. "
                "That is the ONE implementation of 'what files are under this root', and the only one "
                "that sees a mounted .dpak.";
        EXPECT_EQ( code.find( "recursive_directory_iterator" ), std::string::npos )
             << file
             << " walks directories itself. Two enumerations are two answers to what the project "
                "contains, and the second one is always the one that forgets the pak.";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------------------------------
// 6. THE SERVICE ASKS THE BYTE BUDGET AS BACKGROUND WORK.
//
// Background keeps the main view's forecast free; a UserSurface entitlement here would let a queue of
// thumbnails eat the memory the next view a person opens needs. Asserted on the source, like section 5:
// a refusal needs a device near its budget, which no suite can arrange (Desert/Tests/Engine/ViewBudget
// proves the rule itself).
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailSweep, TheServiceAsksTheViewBudgetAsBackgroundWork )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";
    const std::string code = ReadFile( root + "Editor/Source/Editor/Widgets/ThumbnailService.cpp" );
    ASSERT_FALSE( code.empty() );

    EXPECT_NE( code.find( "MayCreateView(" ), std::string::npos )
         << "ThumbnailService builds its renderer without asking the view budget";
    EXPECT_NE( code.find( "Demand::Background" ), std::string::npos )
         << "ThumbnailService asks the view budget, but not as Background work";
    EXPECT_EQ( code.find( "Demand::UserSurface" ), std::string::npos )
         << "ThumbnailService claims the UserSurface entitlement: thumbnails would take the last byte";
}
