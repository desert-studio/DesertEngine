#include <Editor/Widgets/ThumbnailEncode.hpp>
#include <Editor/Widgets/ThumbnailWarmup.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <map>

// THUMB3: which pictures the splash warms, and the rule that paces the capture queue.
namespace Warmup = Desert::Editor::ThumbnailWarmup;
using Desert::Editor::ThumbnailEncode::CaptureBudget;

namespace
{
    Common::AssetHandle H( uint64_t v )
    {
        return Common::AssetHandle{ v };
    }

    // A capture queue driven the way ThumbnailService::TickCapture drives it: once per frame, the budget
    // repaid, the renderer ticked (charged), then a dispatch only when MayDispatch says so.
    struct Simulation
    {
        struct Req
        {
            int  Id;
            bool Warm;
        };
        std::deque<Req>  Queue;
        CaptureBudget    Budget;
        int              InFlightFrames = 0; // frames left on the capture in flight
        std::vector<int> Dispatched;
        std::vector<int> DispatchesPerFrame;

        void Frame( Warmup::CaptureScope scope, int captureFrames, double tickMs )
        {
            Budget.EndFrame();
            if ( InFlightFrames > 0 )
            {
                Budget.Spend( tickMs );
                --InFlightFrames;
            }
            int started = 0;
            // Asked as often as a careless caller might ask: the rule, not the loop, must keep it to one.
            for ( int attempt = 0; attempt < 3; ++attempt )
            {
                const bool frontWarm = !Queue.empty() && Queue.front().Warm;
                if ( Queue.empty() ||
                     !Warmup::MayDispatch( Budget.MayDispatch(), InFlightFrames > 0, scope, frontWarm ) )
                    break;
                Dispatched.push_back( Queue.front().Id );
                Queue.pop_front();
                InFlightFrames = captureFrames;
                ++started;
            }
            DispatchesPerFrame.push_back( started );
        }
    };
} // namespace

TEST( ThumbnailWarmup, TheWarmListIsTheScenesMaterialAndMeshRootsByPath )
{
    const std::map<uint64_t, std::string> files = {
         { 1, "/p/Assets/Materials/B.demat" },        { 2, "/p/Assets/Meshes/Rock.stmesh" },
         { 3, "/p/Assets/Materials/A.demat" },        { 4, "" }, // a root no file names
         { 5, "/p/Assets/Textures/T_Checker.detex" },
    };
    const auto pathFor = [&]( const Common::AssetHandle& h ) -> std::filesystem::path
    { return files.at( static_cast<uint64_t>( h ) ); };

    const std::vector<Warmup::WarmItem> warm =
         Warmup::SceneWarmList( { H( 1 ), H( 2 ), H( 3 ), H( 4 ), H( 5 ), H( 1 ), H( 2 ) }, pathFor );
    const std::vector<Warmup::WarmItem> expected = {
         { "/p/Assets/Materials/A.demat", Warmup::WarmKind::Material },
         { "/p/Assets/Materials/B.demat", Warmup::WarmKind::Material },
         { "/p/Assets/Meshes/Rock.stmesh", Warmup::WarmKind::Mesh },
    };
    EXPECT_EQ( warm, expected ) << "every material and mesh root once, sorted; textures and nameless roots out";
    EXPECT_TRUE( Warmup::SceneWarmList( {}, pathFor ).empty() );
}

TEST( ThumbnailWarmup, TheSplashWarmsTheScenesSubjectsThenTheFoldersUncapturedTiles )
{
    using Warmup::WarmItem;
    using Warmup::WarmKind;
    const std::vector<WarmItem> scene = {
         { "/p/Assets/Materials/A.demat", WarmKind::Material },
         { "/p/Assets/Meshes/Rock.stmesh", WarmKind::Mesh },
    };
    const std::vector<WarmItem> folder = {
         { "/p/Assets/Meshes/Crate.fbx", WarmKind::Mesh },       // no picture on disk
         { "/p/Assets/Meshes/Barrel.fbx", WarmKind::Mesh },      // fresh picture
         { "/p/Assets/Meshes/Rock.stmesh", WarmKind::Mesh },     // the scene already warms it
         { "/p/Assets/Materials/Stale.demat", WarmKind::Material }, // stale picture
    };
    const std::map<std::string, bool> needs = {
         { "/p/Assets/Meshes/Crate.fbx", true },
         { "/p/Assets/Meshes/Barrel.fbx", false },
         { "/p/Assets/Meshes/Rock.stmesh", true },
         { "/p/Assets/Materials/Stale.demat", true },
         { "/p/Assets/Materials/A.demat", false },
    };
    const auto needsCapture = [&]( const WarmItem& item ) { return needs.at( item.Path ); };

    const std::vector<WarmItem> warm = Warmup::SplashWarmList( scene, folder, needsCapture );
    const std::vector<WarmItem> expected = {
         { "/p/Assets/Materials/A.demat", WarmKind::Material },
         { "/p/Assets/Meshes/Rock.stmesh", WarmKind::Mesh },
         { "/p/Assets/Meshes/Crate.fbx", WarmKind::Mesh },
         { "/p/Assets/Materials/Stale.demat", WarmKind::Material },
    };
    EXPECT_EQ( warm, expected ) << "scene first (fresh or not: its own path prefetches it), then the folder's "
                                   "uncaptured tiles in folder order, each once; a fresh tile is not a capture";
    EXPECT_EQ( Warmup::SplashWarmList( {}, folder, []( const WarmItem& ) { return false; } ).size(), 0u );
}

TEST( ThumbnailWarmup, AtMostOneCaptureStartsPerFrameAndNoneWhileOneIsInFlight )
{
    Simulation sim;
    for ( int i = 0; i < 20; ++i )
        sim.Queue.push_back( { i, false } );
    for ( int frame = 0; frame < 400; ++frame )
        sim.Frame( Warmup::CaptureScope::Everything, 6, 1.0 );

    ASSERT_EQ( sim.Dispatched.size(), 20u ) << "the queue must drain";
    int lastStart = -100;
    for ( std::size_t f = 0; f < sim.DispatchesPerFrame.size(); ++f )
    {
        EXPECT_LE( sim.DispatchesPerFrame[f], 1 ) << "frame " << f;
        if ( sim.DispatchesPerFrame[f] > 0 )
        {
            EXPECT_GE( static_cast<int>( f ) - lastStart, 6 )
                 << "frame " << f << " started over a capture in flight";
            lastStart = static_cast<int>( f );
        }
    }
}

TEST( ThumbnailWarmup, AnExpensiveCaptureSpreadsTheNextOnesOverFrames )
{
    // A capture charging 12 ms a frame owes more than one frame's allowance: the next one waits for the
    // debt to be repaid (bounded by CaptureBudget::kMaxWaitFrames) instead of starting back to back.
    Simulation sim;
    for ( int i = 0; i < 5; ++i )
        sim.Queue.push_back( { i, false } );
    for ( int frame = 0; frame < 200; ++frame )
        sim.Frame( Warmup::CaptureScope::Everything, 2, 12.0 );
    ASSERT_EQ( sim.Dispatched.size(), 5u );
    std::vector<int> starts;
    for ( std::size_t f = 0; f < sim.DispatchesPerFrame.size(); ++f )
        if ( sim.DispatchesPerFrame[f] > 0 )
            starts.push_back( static_cast<int>( f ) );
    for ( std::size_t i = 1; i < starts.size(); ++i )
    {
        EXPECT_GT( starts[i] - starts[i - 1], 2 ) << "no repayment between captures " << i - 1 << " and " << i;
        EXPECT_LE( starts[i] - starts[i - 1], 2 + CaptureBudget::kMaxWaitFrames + 1 );
    }
}

TEST( ThumbnailWarmup, OnTheSplashOnlyTheScenesCapturesStart )
{
    Simulation sim;
    sim.Queue = { { 10, true }, { 11, true }, { 20, false }, { 21, false } };
    for ( int frame = 0; frame < 100; ++frame )
        sim.Frame( Warmup::CaptureScope::SceneWarmOnly, 4, 1.0 );
    const std::vector<int> onSplash = { 10, 11 };
    EXPECT_EQ( sim.Dispatched, onSplash ) << "the folder's captures must wait for the reveal";

    for ( int frame = 0; frame < 100; ++frame )
        sim.Frame( Warmup::CaptureScope::Everything, 4, 1.0 );
    const std::vector<int> all = { 10, 11, 20, 21 };
    EXPECT_EQ( sim.Dispatched, all );
}

// A skinned mesh a scene uses is warmed as a POSE (THM1n-6): its bind pose, not a static capture.
TEST( ThumbnailWarmup, ASkinnedMeshIsWarmedAsAPose )
{
    EXPECT_EQ( Desert::Editor::ThumbnailWarmup::WarmKindOf( "Assets/Hero/Hero.skmesh" ),
               Desert::Editor::ThumbnailWarmup::WarmKind::Pose );
}
