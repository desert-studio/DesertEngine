// The per-user driver pipeline cache (PSO1): path, device header, in-run persist schedule. No device.
#include <gtest/gtest.h>

#include <Engine/Assets/ContentGate.hpp>
#include <Engine/Graphic/PipelineBuilds.hpp>
#include <Engine/Graphic/PipelineCacheFile.hpp>

#include <Engine/Graphic/MaterialPipelineStates.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr auto Engine   = Desert::Graphic::PipelineRole::Engine;
    constexpr auto Material = Desert::Graphic::PipelineRole::Material;
    using State             = Desert::Graphic::MaterialPipelineState;

    // A repository file's text, found by walking up from the working directory; empty when not found.
    std::string ReadSource( const std::string& relative )
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up, prefix += "../" )
        {
            const std::ifstream in( prefix + relative, std::ios::binary );
            if ( !in )
                continue;
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }
        return {};
    }
} // namespace

using namespace Desert::Graphic::PipelineCacheFile;
using namespace std::chrono_literals;

namespace
{
    DeviceIdentity Gpu()
    {
        DeviceIdentity id{ 0x106b, 0x1a0, 0x00020001, {} };
        for ( size_t i = 0; i < id.CacheUuid.size(); ++i )
            id.CacheUuid[i] = static_cast<uint8_t>( i * 17 );
        return id;
    }
} // namespace

TEST( PipelineCacheFile, GameKeepsItInTheProductsOwnDirectory )
{
    // GameUserDirectory(Name) is already this product's, so no project segment is added under it.
    const std::filesystem::path player = "/Users/p/Library/Application Support/My Game";
    EXPECT_EQ( Directory( Host::Game, player, "My Game" ), player / "PipelineCache" );
}

TEST( PipelineCacheFile, EditorKeepsOneFolderPerProjectName )
{
    const auto dir = Directory( Host::Editor, "/home/u/.desertengine", "Sandbox" );
    EXPECT_EQ( dir, std::filesystem::path( "/home/u/.desertengine/PipelineCache/Sandbox" ) );
    EXPECT_NE( Directory( Host::Editor, "/home/u/.desertengine", "Other" ), dir );
}

TEST( PipelineCacheFile, EditorProjectNameIsOneSafeSegment )
{
    // A `.deproj` Name is data: separators, relative names and Windows-refused characters must not leave
    // <userDir>/PipelineCache or produce a path Windows cannot create.
    const std::filesystem::path root = "/home/u/.desertengine/PipelineCache";
    EXPECT_EQ( Directory( Host::Editor, "/home/u/.desertengine", "../../etc/x" ), root / ".._.._etc_x" );
    EXPECT_EQ( Directory( Host::Editor, "/home/u/.desertengine", "a\\b:c<d>e\"f|g?h*" ),
               root / "a_b_c_d_e_f_g_h_" );
    EXPECT_EQ( Directory( Host::Editor, "/home/u/.desertengine", ".." ), root / "DesertGame" );
    EXPECT_EQ( Directory( Host::Editor, "/home/u/.desertengine", "" ), root / "DesertGame" );
}

TEST( PipelineCacheFile, HostIsUndeclaredUntilMainDeclaresIt )
{
    EXPECT_FALSE( DeclaredHost().has_value() );
    DeclareHost( Host::Game );
    EXPECT_EQ( DeclaredHost(), Host::Game );
}

TEST( PipelineCacheFile, FileNameCarriesVendorDeviceDriverAndUuid )
{
    const DeviceIdentity id = Gpu();
    EXPECT_EQ( FileName( id ), "0000106b-000001a0-00020001-00112233445566778899aabbccddeeff.bin" );

    DeviceIdentity newDriver = id;
    newDriver.Driver += 1;
    EXPECT_NE( FileName( newDriver ), FileName( id ) ) << "a driver update must not overwrite the old blob";
    DeviceIdentity otherGpu = id;
    otherGpu.Device += 1;
    EXPECT_NE( FileName( otherGpu ), FileName( id ) );
    DeviceIdentity otherUuid = id;
    otherUuid.CacheUuid[15] ^= 1;
    EXPECT_NE( FileName( otherUuid ), FileName( id ) );
}

TEST( PipelineCacheFile, RoundTripsTheDriverBytes )
{
    const std::string blob( "\x01\x02\0driver-bytes", 15 );
    const auto        decoded     = Decode( Gpu(), Encode( Gpu(), blob ) );
    const auto&       decodedBlob = decoded.Blob;
    if ( !decodedBlob.has_value() )
        FAIL() << decoded.Refusal;
    EXPECT_EQ( *decodedBlob, blob );
    EXPECT_TRUE( decoded.Refusal.empty() );
}

TEST( PipelineCacheFile, RefusesAnotherDevicesFileWithAReason )
{
    DeviceIdentity other = Gpu();
    other.Driver         = 7;
    const auto decoded   = Decode( Gpu(), Encode( other, "blob" ) );
    EXPECT_FALSE( decoded.Blob );
    EXPECT_NE( decoded.Refusal.find( "written for" ), std::string::npos ) << decoded.Refusal;
}

TEST( PipelineCacheFile, RefusesTornCorruptAndForeignFiles )
{
    const std::string file = Encode( Gpu(), "0123456789" );

    EXPECT_FALSE( Decode( Gpu(), file.substr( 0, file.size() - 1 ) ).Blob ) << "truncated payload";
    EXPECT_FALSE( Decode( Gpu(), file.substr( 0, 8 ) ).Blob ) << "truncated header";

    std::string flipped = file;
    flipped.back() ^= 0x20;
    const auto corrupt = Decode( Gpu(), flipped );
    EXPECT_FALSE( corrupt.Blob );
    EXPECT_NE( corrupt.Refusal.find( "hash" ), std::string::npos ) << corrupt.Refusal;

    std::string foreign = file;
    foreign[0]          = 'X';
    EXPECT_FALSE( Decode( Gpu(), foreign ).Blob ) << "a raw driver blob from the old layout is not ours";
}

TEST( PipelineCacheFile, PersistsDuringTheRunNotOnlyAtExit )
{
    PersistSchedule schedule( 2s );
    const auto      t0 = std::chrono::steady_clock::time_point{} + 100s;

    EXPECT_FALSE( schedule.Due( 0, t0 ) ) << "nothing built, nothing to write";
    EXPECT_TRUE( schedule.Due( 5, t0 ) ) << "the first write after start is not throttled";
    EXPECT_FALSE( schedule.Due( 9, t0 + 1s ) ) << "throttled inside the interval";
    EXPECT_TRUE( schedule.Due( 9, t0 + 2s ) ) << "due once the interval has passed and pipelines were built";
    EXPECT_FALSE( schedule.Due( 9, t0 + 60s ) ) << "no new pipelines, no rewrite";
    EXPECT_TRUE( schedule.Due( 10, t0 + 60s ) );
}

TEST( PipelineBuilds, CountsPendingAndStartedMonotonically )
{
    Desert::Graphic::PipelineBuilds::Tracker builds;
    builds.OnStarted( Engine );
    builds.OnStarted( Engine );
    EXPECT_EQ( builds.Pending( Engine ), 2u );
    builds.OnFinished( Engine );
    EXPECT_EQ( builds.Pending( Engine ), 1u );
    EXPECT_EQ( builds.Started( Engine ), 2u )
         << "started never goes down: ContentGate reads it as 'asked this frame'";
}

TEST( PipelineBuilds, CountsEngineAndMaterialCompilesApart )
{
    Desert::Graphic::PipelineBuilds::Tracker builds;
    builds.OnStarted( Engine );
    builds.OnStarted( Material );
    builds.OnStarted( Material );
    EXPECT_EQ( builds.Pending( Engine ), 1u );
    EXPECT_EQ( builds.Pending( Material ), 2u );
    EXPECT_EQ( builds.Started( Material ), 2u );
    builds.OnFinished( Material );
    EXPECT_EQ( builds.Pending( Engine ), 1u ) << "a material landing must not release the engine's count";
}

TEST( PipelineBuilds, WaitIdleReturnsOnlyAfterTheLastCompileFinished )
{
    Desert::Graphic::PipelineBuilds::Tracker builds;
    builds.OnStarted( Material );
    std::atomic<bool> finished{ false };
    std::thread       worker(
         [&]
         {
             std::this_thread::sleep_for( 50ms );
             finished = true;
             builds.OnFinished( Material );
         } );
    builds.WaitIdle();
    EXPECT_TRUE( finished ) << "hot reload would replace a shader a compile is still reading (material ones too)";
    worker.join();
}

// The editor's splash and the runtime's loading screen: the gate is fed loader + ENGINE pipelines
// (ContentWorkNow), so a system pass still in the driver keeps the world Loading exactly like an asset read would.
TEST( PipelineBuilds, ContentGateWaitsForAnEnginePipelineStillInTheDriver )
{
    using Desert::Assets::ContentGate;
    using Desert::Assets::ContentState;
    Desert::Graphic::PipelineBuilds::Tracker builds;
    ContentGate                              gate( ContentState::Ready );
    const size_t                             assetsOutstanding = 0;
    const uint64_t                           assetsStarted     = 7;
    gate.BeginWorld( assetsStarted + builds.Started( Engine ) );

    builds.OnStarted( Engine ); // the first frame built the G-buffer pass: its pipeline went to a worker
    for ( int frame = 0; frame < 10; ++frame )
        EXPECT_FALSE(
             gate.Tick( assetsOutstanding + builds.Pending( Engine ), assetsStarted + builds.Started( Engine ) ) );
    EXPECT_TRUE( gate.Loading() ) << "opened while a system pipeline was still compiling: that frame skips a pass";

    builds.OnFinished( Engine );
    bool opened = false;
    for ( int frame = 0; frame < 3 && !opened; ++frame )
        opened =
             gate.Tick( assetsOutstanding + builds.Pending( Engine ), assetsStarted + builds.Started( Engine ) );
    EXPECT_TRUE( opened );
}

// AL1-12: the gate is NOT held by a material pipeline — its meshes draw the default surface meanwhile.
TEST( PipelineBuilds, ContentWorkCountsOnlyEnginePipelines )
{
    const std::string work = ReadSource( "Desert/Desert/Source/Engine/Assets/ContentWork.hpp" );
    ASSERT_FALSE( work.empty() ) << "run from the repository root or below it";
    EXPECT_NE( work.find( "pipelines.Pending( Graphic::PipelineRole::Engine )" ), std::string::npos );
    EXPECT_NE( work.find( "pipelines.Started( Graphic::PipelineRole::Engine )" ), std::string::npos );
    EXPECT_EQ( work.find( "PipelineRole::Material" ), std::string::npos )
         << "the reveal would wait seconds for material pipelines on a cold cache again";
}

// Census: no startup stage compiles a material pipeline. The only caller of the material path is the mesh
// renderer's material draw; boot, the hosts and the scene renderer do not reach it.
TEST( MaterialPipelines, NoStartupStageCompilesAMaterialPipeline )
{
    const char* startup[] = { "Desert/Desert/Source/Engine/Assets/BootContent.cpp",
                              "Editor/Source/EditorLayer.cpp", "Runtime/Source/RuntimeLayer.cpp",
                              "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" };
    for ( const char* file : startup )
    {
        const std::string text = ReadSource( file );
        ASSERT_FALSE( text.empty() ) << file;
        EXPECT_EQ( text.find( "GetOrCreateMaterial" ), std::string::npos ) << file;
        EXPECT_EQ( text.find( "CreateAsync" ), std::string::npos ) << file;
    }
    const std::string mesh =
         ReadSource( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp" );
    EXPECT_NE( mesh.find( "GetOrCreateMaterial" ), std::string::npos )
         << "the material draw lost its on-demand path";
    const std::string factory = ReadSource( "Desert/Desert/Source/Engine/Graphic/Materials/MaterialFactory.cpp" );
    EXPECT_NE( factory.find( "MaterialPipelineRequests::Get().Request( shaderName )" ), std::string::npos )
         << "a material's pipeline is requested when it LOADS, not at its first draw";
}

TEST( MaterialPipelines, RequestedCompilingReadyAndTheDrawPicksTheDefaultUntilReady )
{
    Desert::Graphic::MaterialPipelineTracker tracker;
    tracker.Request( "MatA" );
    EXPECT_EQ( tracker.StateOf( "MatA" ), State::Requested );
    EXPECT_FALSE( tracker.StateOf( "Other" ).has_value() );

    tracker.OnCompiling( "MatA" );
    EXPECT_EQ( tracker.StateOf( "MatA" ), State::Compiling );
    for ( int frame = 0; frame < 5; ++frame )
        EXPECT_FALSE( tracker.Choose( "MatA" ).UseOwnPipeline )
             << "a compiling material draws the default surface";

    tracker.Request( "MatA" ); // a second load of the same material does not move it back
    EXPECT_EQ( tracker.StateOf( "MatA" ), State::Compiling );

    tracker.OnReady( "MatA" );
    EXPECT_EQ( tracker.StateOf( "MatA" ), State::Ready );
    EXPECT_TRUE( tracker.Choose( "MatA" ).UseOwnPipeline );
    EXPECT_EQ( tracker.Count( State::Ready ), 1u );
}

TEST( MaterialPipelines, FallbackIsAnnouncedOncePerMaterialPerState )
{
    Desert::Graphic::MaterialPipelineTracker tracker;
    tracker.OnCompiling( "MatA" );
    tracker.OnCompiling( "MatB" );
    EXPECT_TRUE( tracker.Choose( "MatA" ).Announce );
    EXPECT_FALSE( tracker.Choose( "MatA" ).Announce ) << "logged every frame instead of once";
    EXPECT_TRUE( tracker.Choose( "MatB" ).Announce ) << "one material's log must not silence another's";

    tracker.OnFailed( "MatA" );
    const auto failed = tracker.Choose( "MatA" );
    EXPECT_FALSE( failed.UseOwnPipeline ) << "a refused material draws the default surface, not nothing";
    EXPECT_TRUE( failed.Announce ) << "the failure is a new state and is said once";
    EXPECT_FALSE( tracker.Choose( "MatA" ).Announce );
}

TEST( MaterialPipelines, HotReloadSendsAReadyMaterialBackToTheDefaultWhileItRecompiles )
{
    Desert::Graphic::MaterialPipelineTracker tracker;
    tracker.OnReady( "MatA" );
    EXPECT_TRUE( tracker.Choose( "MatA" ).UseOwnPipeline );
    tracker.OnCompiling( "MatA" );
    EXPECT_FALSE( tracker.Choose( "MatA" ).UseOwnPipeline );
    tracker.OnReady( "MatA" );
    EXPECT_TRUE( tracker.Choose( "MatA" ).UseOwnPipeline );
}

TEST( MaterialPipelines, OnLoadRequestsReachEveryRendererFromItsOwnCursor )
{
    Desert::Graphic::MaterialPipelineRequests requests;
    requests.Request( "MatA" );
    requests.Request( "MatB" );
    requests.Request( "MatA" );
    size_t main = 0;
    EXPECT_EQ( requests.Since( main ), ( std::vector<std::string>{ "MatA", "MatB" } ) );
    EXPECT_TRUE( requests.Since( main ).empty() ) << "a renderer is handed each request once";
    requests.Request( "MatC" );
    size_t preview = 0; // a viewport created later still precaches what loaded before it existed
    EXPECT_EQ( requests.Since( preview ), ( std::vector<std::string>{ "MatA", "MatB", "MatC" } ) );
    EXPECT_EQ( requests.Since( main ), ( std::vector<std::string>{ "MatC" } ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
