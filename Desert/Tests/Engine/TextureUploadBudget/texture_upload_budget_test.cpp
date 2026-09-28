// AM2: textures reach the GPU through a per-frame byte budget, and the on-demand path never cooks on the main
// thread.
//
// Two halves. The queue tests pin the budget's state machine (order, the budget edge, the one-item floor,
// bytes accounting, cross-thread pushes). The census reads the service's source: a platform-data read or a
// cook reachable from the main-thread load path is exactly the 1.2 s THUMB3 measured on a DDC miss, and it
// compiles and passes every behavioural test while it stalls the frame.

#include <Engine/Runtime/Services/Texture/TextureUploadQueue.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    using Queue = Desert::Runtime::TextureUploadQueue<uint64_t, int>;

    Queue::Item Make( uint64_t handle, uint64_t bytes, uint64_t ticket = 1 )
    {
        return Queue::Item{ handle, ticket, bytes, static_cast<int>( handle ) };
    }

    std::vector<uint64_t> Handles( const std::vector<Queue::Item>& items )
    {
        std::vector<uint64_t> out;
        for ( const auto& item : items )
            out.push_back( item.Handle );
        return out;
    }

    constexpr uint64_t kMiB = 1024ull * 1024ull;

    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( ( prefix / "Desert/Desert/premake5.lua" ).string() ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const fs::path& file )
    {
        const std::ifstream in( file.string() );
        if ( !in )
            return {};
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The body of the definition `<owner>::<name>(`, braces matched; empty when there is none.
    std::string Body( const std::string& source, const std::string& qualifiedName )
    {
        const auto at = source.find( qualifiedName + "(" );
        if ( at == std::string::npos )
            return {};
        const auto open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};
        int depth = 0;
        for ( std::size_t i = open; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                return source.substr( open, i - open + 1 );
        }
        return {};
    }

    std::size_t Count( const std::string& text, const std::string& token )
    {
        std::size_t n = 0;
        for ( auto at = text.find( token ); at != std::string::npos; at = text.find( token, at + token.size() ) )
            ++n;
        return n;
    }

    const std::string kService = "Desert/Desert/Source/Engine/Runtime/Services/Texture/TextureService.cpp";
    const std::string kTexture = "Desert/Desert/Source/Engine/Graphic/Texture.cpp";
    const std::string kLoop    = "Desert/Desert/Source/Engine/Core/Application.cpp";
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// The budget's state machine.

TEST( TextureUploadBudget, TakesInArrivalOrderUpToTheBudget )
{
    Queue queue;
    queue.Push( Make( 1, 3 * kMiB ) );
    queue.Push( Make( 2, 3 * kMiB ) );
    queue.Push( Make( 3, 3 * kMiB ) );

    EXPECT_EQ( Handles( queue.TakeWithinBudget( 8 * kMiB ) ), ( std::vector<uint64_t>{ 1, 2 } ) )
         << "9 MiB waiting under an 8 MiB budget: the third texture belongs to the next frame";
    EXPECT_EQ( queue.Pending(), 1u );
    EXPECT_EQ( Handles( queue.TakeWithinBudget( 8 * kMiB ) ), ( std::vector<uint64_t>{ 3 } ) );
    EXPECT_EQ( queue.Pending(), 0u );
}

TEST( TextureUploadBudget, AnItemExactlyFillingTheBudgetIsTaken )
{
    Queue queue;
    queue.Push( Make( 1, 4 * kMiB ) );
    queue.Push( Make( 2, 4 * kMiB ) );
    queue.Push( Make( 3, 1 ) );
    EXPECT_EQ( Handles( queue.TakeWithinBudget( 8 * kMiB ) ), ( std::vector<uint64_t>{ 1, 2 } ) );
}

TEST( TextureUploadBudget, AnItemLargerThanTheBudgetStillGoesAloneOnItsFrame )
{
    Queue queue;
    queue.Push( Make( 1, 20 * kMiB ) );
    queue.Push( Make( 2, 1 * kMiB ) );

    // Without the one-item floor the 20 MiB texture would wait forever and everything behind it with it.
    EXPECT_EQ( Handles( queue.TakeWithinBudget( 8 * kMiB ) ), ( std::vector<uint64_t>{ 1 } ) );
    EXPECT_EQ( Handles( queue.TakeWithinBudget( 8 * kMiB ) ), ( std::vector<uint64_t>{ 2 } ) );
}

TEST( TextureUploadBudget, AnEmptyQueueTakesNothingAndAFrameNeverTakesZeroWhileSomethingWaits )
{
    Queue queue;
    EXPECT_TRUE( queue.TakeWithinBudget( 8 * kMiB ).empty() );
    queue.Push( Make( 1, 5 ) );
    EXPECT_EQ( queue.TakeWithinBudget( 0 ).size(), 1u ) << "a zero budget must not stall the queue";
}

TEST( TextureUploadBudget, PendingBytesFollowPushesAndTakes )
{
    Queue queue;
    queue.Push( Make( 1, 3 * kMiB ) );
    queue.Push( Make( 2, 6 * kMiB ) );
    EXPECT_EQ( queue.PendingBytes(), 9 * kMiB );
    (void)queue.TakeWithinBudget( 4 * kMiB );
    EXPECT_EQ( queue.PendingBytes(), 6 * kMiB );
    queue.Clear();
    EXPECT_EQ( queue.PendingBytes(), 0u );
    EXPECT_EQ( queue.Pending(), 0u );
}

TEST( TextureUploadBudget, ItemsKeepTheirTicketAndPayload )
{
    Queue queue;
    queue.Push( Make( 7, 1, 42 ) );
    const auto taken = queue.TakeWithinBudget( 8 * kMiB );
    ASSERT_EQ( taken.size(), 1u );
    EXPECT_EQ( taken[0].Ticket, 42u ) << "the service tells a stale cook from a live one by this ticket";
    EXPECT_EQ( taken[0].Data, 7 );
}

TEST( TextureUploadBudget, WorkersPushWhileTheFrameTakes )
{
    Queue                    queue;
    constexpr int            kThreads = 4;
    constexpr int            kEach    = 500;
    std::vector<std::thread> workers;
    for ( int t = 0; t < kThreads; ++t )
        workers.emplace_back(
             [&queue, t]
             {
                 for ( int i = 0; i < kEach; ++i )
                     queue.Push( Make( static_cast<uint64_t>( t * kEach + i ), 1 ) );
             } );
    std::size_t taken = 0;
    while ( taken < static_cast<std::size_t>( kThreads * kEach ) )
        taken += queue.TakeWithinBudget( 64 ).size();
    for ( auto& worker : workers )
        worker.join();
    EXPECT_EQ( taken, static_cast<std::size_t>( kThreads * kEach ) );
    EXPECT_EQ( queue.PendingBytes(), 0u );
}

// ---------------------------------------------------------------------------------------------------------------
// The census: no cook and no platform-data read on the main-thread load path.

TEST( TextureUploadBudgetCensus, TheServiceNeverReadsPlatformDataOnTheMainThread )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from the repository or below it";
    const std::string service = ReadAll( root / kService );
    ASSERT_FALSE( service.empty() ) << kService;

    // The synchronous doors to the DDC read and the cook. None may appear in the service at all.
    for ( const char* door : { "CreateFromAsset", "LoadTexturePlatformData", "BuildPlatformData" } )
        EXPECT_EQ( Count( service, door ), 0u )
             << kService << " calls " << door << " -- a DDC read (and on a miss, a cook) on the main thread";

    // The one read there is lives in the job BeginCook submits, and nowhere else in the file.
    const std::string beginCook = Body( service, "TextureService::BeginCook" );
    ASSERT_FALSE( beginCook.empty() ) << "TextureService::BeginCook is gone; the census lost its anchor";
    const auto submit = beginCook.find( "JobSystem::Get().Submit(" );
    ASSERT_NE( submit, std::string::npos ) << "BeginCook no longer hands the cook to a worker";
    EXPECT_EQ( Count( service, "ReadCooked" ), 1u ) << "ReadCooked is called outside BeginCook's job";
    EXPECT_GT( beginCook.find( "ReadCooked" ), submit ) << "ReadCooked runs before the job is submitted";

    // The GPU half runs from the budgeted pump only, and the eager Create2D stays in Register (import-made
    // assets whose DDC entry the importer has just written).
    EXPECT_EQ( Count( service, "CreateFromCooked" ), 1u );
    EXPECT_NE( Body( service, "TextureService::FinishCook" ).find( "CreateFromCooked" ), std::string::npos );
    EXPECT_NE( Body( service, "TextureService::PumpUploads" ).find( "FinishCook" ), std::string::npos );
    EXPECT_NE( Body( service, "TextureService::PumpUploads" ).find( "TakeWithinBudget" ), std::string::npos )
         << "the pump no longer respects the per-frame budget";
    EXPECT_EQ( Count( service, "Create2D" ), 1u );
    EXPECT_NE( Body( service, "TextureService::Register" ).find( "Create2D" ), std::string::npos );
}

TEST( TextureUploadBudgetCensus, TheGpuHalfDoesNotReadAndTheFrameLoopPumps )
{
    const fs::path    root    = RepoRoot();
    const std::string texture = ReadAll( root / kTexture );
    ASSERT_FALSE( texture.empty() ) << kTexture;
    const std::string gpuHalf = Body( texture, "Texture2D::CreateFromCooked" );
    ASSERT_FALSE( gpuHalf.empty() );
    EXPECT_EQ( gpuHalf.find( "LoadTexturePlatformData" ), std::string::npos );
    EXPECT_EQ( gpuHalf.find( "DecodeTextureBinary" ), std::string::npos );
    EXPECT_NE( Body( texture, "Texture2D::ReadCooked" ).find( "LoadTexturePlatformData" ), std::string::npos )
         << "positive control: the CPU half is where the read lives";

    const std::string loop = ReadAll( root / kLoop );
    EXPECT_EQ( Count( loop, "GetTextureService()->PumpUploads()" ), 1u )
         << "nothing pumps the uploads each frame: every on-demand texture would stay Pending forever";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
