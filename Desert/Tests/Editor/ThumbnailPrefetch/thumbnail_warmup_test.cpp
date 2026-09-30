#include <Editor/Widgets/ThumbnailEncode.hpp>
#include <Editor/Widgets/ThumbnailWarmup.hpp>

#include <gtest/gtest.h>

#include <algorithm>
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
         { "/p/Assets/Meshes/Crate.fbx", WarmKind::Mesh },          // no picture on disk
         { "/p/Assets/Meshes/Barrel.fbx", WarmKind::Mesh },         // fresh picture
         { "/p/Assets/Meshes/Rock.stmesh", WarmKind::Mesh },        // the scene already warms it
         { "/p/Assets/Materials/Stale.demat", WarmKind::Material }, // stale picture
    };
    const std::map<std::string, bool> needs = {
         { "/p/Assets/Meshes/Crate.fbx", true },   { "/p/Assets/Meshes/Barrel.fbx", false },
         { "/p/Assets/Meshes/Rock.stmesh", true }, { "/p/Assets/Materials/Stale.demat", true },
         { "/p/Assets/Materials/A.demat", false },
    };
    const auto needsCapture = [&]( const WarmItem& item ) { return needs.at( item.Path ); };

    const std::vector<WarmItem> warm     = Warmup::SplashWarmList( scene, folder, needsCapture );
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

// THM1n-13 (owner 09-29 "all assets on the splash"): the splash warms EVERY picture of the project, read from the
// content registry — one row of each kind here. A kind whose rows the list stops reading (a skipped kind, a
// producer that no longer maps) drops its row and this goes red; a kind with no picture never appears.
TEST( ThumbnailWarmup, TheProjectListIsEveryRegistryRowWithAPictureProducer )
{
    using ::Common::Content::ContentKind;
    using Warmup::WarmItem;
    using Warmup::WarmKind;
    const std::map<ContentKind, std::vector<std::filesystem::path>> rows = {
         { ContentKind::StaticMesh, { "/p/Assets/Meshes/Rock.stmesh" } },
         { ContentKind::SkinnedMesh, { "/p/Assets/Meshes/Fox.skmesh" } },
         { ContentKind::Skeleton, { "/p/Assets/Meshes/Fox.skeleton" } },
         { ContentKind::Animation, { "/p/Assets/Meshes/Fox_Walk.anim" } },
         { ContentKind::Texture, { "/p/Assets/Textures/Bark.detex" } },
         { ContentKind::Material, { "/p/Assets/Materials/Bark.demat" } },
         { ContentKind::Skybox, { "/p/Assets/Skybox/Dusk.detex" } },
         { ContentKind::Shader, { "/p/Shaders/Lit.glsl" } },
         { ContentKind::CloudNoiseVolume, { "/p/Assets/Clouds/N.dcnv" } },
         { ContentKind::CloudType, { "/p/Assets/Clouds/Cumulus.decloudtype" } },
         { ContentKind::CloudModellingVolume, { "/p/Assets/Clouds/V.dcmv" } },
         { ContentKind::CloudLayout, { "/p/Assets/Clouds/L.dclayout" } },
         { ContentKind::UITheme, { "/p/Assets/UI/Dark.detheme" } },
         { ContentKind::FoliageType, { "/p/Assets/Foliage/Fern.defoliage" } },
         { ContentKind::Scene, { "/p/Scenes/Main.desce" } },
         { ContentKind::Redirector, { "/p/Assets/Materials/Old.demat" } },
    };
    const auto filesOf = [&]( ContentKind kind )
    {
        const auto it = rows.find( kind );
        return it == rows.end() ? std::vector<std::filesystem::path>{} : it->second;
    };

    const std::vector<WarmItem> expected = {
         { "/p/Assets/Clouds/Cumulus.decloudtype", WarmKind::Painted },
         { "/p/Assets/Clouds/L.dclayout", WarmKind::Painted },
         { "/p/Assets/Clouds/N.dcnv", WarmKind::Painted },
         { "/p/Assets/Clouds/V.dcmv", WarmKind::Painted },
         { "/p/Assets/Foliage/Fern.defoliage", WarmKind::Mesh },
         { "/p/Assets/Materials/Bark.demat", WarmKind::Material },
         { "/p/Assets/Meshes/Fox.skeleton", WarmKind::Pose }, // THM-FIXB: its preview mesh, bind pose
         { "/p/Assets/Meshes/Fox.skmesh", WarmKind::Pose },
         { "/p/Assets/Meshes/Fox_Walk.anim", WarmKind::Pose }, // THM-FIXB: the preview mesh at mid-clip
         { "/p/Assets/Meshes/Rock.stmesh", WarmKind::Mesh },
         { "/p/Assets/Skybox/Dusk.detex", WarmKind::Sky }, // THM-FIXH: typed by its row's kind, photographed
         { "/p/Assets/Textures/Bark.detex", WarmKind::Decoded },
         { "/p/Assets/UI/Dark.detheme", WarmKind::Painted },
    };
    EXPECT_EQ( Warmup::ProjectWarmList( filesOf ), expected )
         << "every registry row whose type has a picture, sorted by path; shaders, scenes and redirector "
            "stubs have none";

    // THM-FIXB: what has no producer is SAID, by kind — the scene here (NotYetProduced); the shader is an icon
    // by decision and the redirector a stub, so neither is listed.
    const std::vector<Warmup::Unproduced> gaps = Warmup::UnproducedKinds( filesOf );
    ASSERT_EQ( gaps.size(), 1U );
    EXPECT_EQ( gaps.front().Kind, ContentKind::Scene );
    EXPECT_EQ( gaps.front().Files, 1U );
    EXPECT_FALSE( gaps.front().Why.empty() );

    // A texture is its own picture: in the project list, never a capture.
    const std::vector<WarmItem> captures =
         Warmup::SplashWarmList( {}, expected, []( const WarmItem& ) { return true; } );
    EXPECT_EQ( captures.size(), expected.size() - 1 );
    EXPECT_TRUE( std::none_of( captures.begin(), captures.end(),
                               []( const WarmItem& item ) { return item.Kind == WarmKind::Decoded; } ) );

    // THM-FIXH (owner 09-29: the window shows when EVERY picture of EVERY kind is ready): a skybox is a capture
    // the splash runs, not one left to the first sight of its tile.
    const WarmItem sky{ "/p/Assets/Skybox/Dusk.detex", WarmKind::Sky };
    EXPECT_NE( std::find( captures.begin(), captures.end(), sky ), captures.end() )
         << "the splash photographs the project's skyboxes";
}

// Every producer that makes a picture has a warm kind — none is left to be captured on sight after the reveal.
TEST( ThumbnailWarmup, EveryRenderedProducerIsWarmedOnTheSplash )
{
    using Desert::Editor::ThumbnailProducers::Producer;
    for ( const Desert::Editor::ThumbnailProducers::Row& row : Desert::Editor::ThumbnailProducers::kTable )
    {
        const bool pictured = row.How != Producer::TypeIcon && row.How != Producer::NotYetProduced;
        EXPECT_EQ( Warmup::WarmKindOfType( row.Type ).has_value(), pictured )
             << "file type " << static_cast<int>( row.Type ) << ": " << row.Why;
    }
}
