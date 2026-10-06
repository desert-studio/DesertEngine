#include <Common/Core/JobSystem.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <vector>

// The pool is a process-global singleton — tests share it, which is exactly how the engine uses it.

TEST( JobSystem, SubmitRunsJobs )
{
    std::atomic<int> counter{ 0 };
    constexpr int    kJobs = 64;

    std::atomic<int> done{ 0 };
    for ( int i = 0; i < kJobs; ++i )
        Common::JobSystem::Get().Submit(
             [&]
             {
                 counter.fetch_add( 1 );
                 done.fetch_add( 1 );
             } );

    while ( done.load() < kJobs )
        std::this_thread::yield();

    EXPECT_EQ( counter.load(), kJobs );
}

TEST( JobSystem, AsyncReturnsValue )
{
    auto fut = Common::JobSystem::Get().Async( [] { return 7 * 6; } );
    EXPECT_EQ( fut.get(), 42 );
}

TEST( JobSystem, ParallelForCoversEveryIndexExactlyOnce )
{
    constexpr size_t      kCount = 10007; // prime: exercises the uneven-chunk remainder path
    std::vector<std::atomic<int>> hits( kCount );

    Common::JobSystem::Get().ParallelFor( kCount, [&]( size_t i ) { hits[i].fetch_add( 1 ); } );

    for ( size_t i = 0; i < kCount; ++i )
        ASSERT_EQ( hits[i].load(), 1 ) << "index " << i;
}

TEST( JobSystem, ParallelForZeroAndOne )
{
    std::atomic<int> calls{ 0 };
    Common::JobSystem::Get().ParallelFor( 0, [&]( size_t ) { calls.fetch_add( 1 ); } );
    EXPECT_EQ( calls.load(), 0 );

    Common::JobSystem::Get().ParallelFor( 1, [&]( size_t ) { calls.fetch_add( 1 ); } );
    EXPECT_EQ( calls.load(), 1 );
}

TEST( JobSystem, ParallelForComputesCorrectSum )
{
    constexpr size_t   kCount = 4096;
    std::vector<long>  values( kCount );

    Common::JobSystem::Get().ParallelFor( kCount,
                                          [&]( size_t i ) { values[i] = static_cast<long>( i ) * 2; } );

    const long sum      = std::accumulate( values.begin(), values.end(), 0L );
    const long expected = static_cast<long>( kCount ) * ( kCount - 1 ); // 2 * sum(0..n-1)
    EXPECT_EQ( sum, expected );
}

TEST( JobSystem, NestedSubmitFromWorker )
{
    // A job submitting another job must not deadlock (Pump-style chaining relies on this).
    std::atomic<bool> innerRan{ false };
    std::atomic<bool> outerDone{ false };

    Common::JobSystem::Get().Submit(
         [&]
         {
             Common::JobSystem::Get().Submit( [&] { innerRan.store( true ); } );
             outerDone.store( true );
         } );

    while ( !outerDone.load() || !innerRan.load() )
        std::this_thread::yield();

    SUCCEED();
}

TEST( JobSystem, ParallelRangesCoversEveryIndexExactlyOnceInContiguousRanges )
{
    constexpr size_t              kCount = 10007; // prime: the last range is short
    constexpr size_t              kGrain = 64;
    std::vector<std::atomic<int>> hits( kCount );
    std::atomic<int>              badRange{ 0 };

    Common::JobSystem::Get().ParallelRanges( kCount, kGrain,
                                             [&]( size_t begin, size_t end )
                                             {
                                                 // The contract is a range, not a pair of loose numbers:
                                                 // a body that trusted `end - begin == grain` would walk
                                                 // off the end of the last one.
                                                 if ( begin >= end || end > kCount || end - begin > kGrain )
                                                     badRange.fetch_add( 1 );
                                                 for ( size_t i = begin; i < end; ++i )
                                                     hits[i].fetch_add( 1 );
                                             } );

    EXPECT_EQ( badRange.load(), 0 );
    for ( size_t i = 0; i < kCount; ++i )
        ASSERT_EQ( hits[i].load(), 1 ) << "index " << i;
}

TEST( JobSystem, ParallelRangesGrainZeroIsOneIndexPerRange )
{
    std::atomic<int> ranges{ 0 };
    std::atomic<int> indices{ 0 };

    Common::JobSystem::Get().ParallelRanges( 8, 0,
                                             [&]( size_t begin, size_t end )
                                             {
                                                 ranges.fetch_add( 1 );
                                                 indices.fetch_add( static_cast<int>( end - begin ) );
                                             } );

    EXPECT_EQ( ranges.load(), 8 );
    EXPECT_EQ( indices.load(), 8 );
}

// THE REGRESSION TEST FOR THE HANG THAT MADE ParallelRanges NECESSARY (Г10).
//
// Every worker is held inside a parallel loop AT THE SAME TIME — not "probably", the barrier makes it
// certain — so every helper job any of them submits is stuck behind workers that are all waiting. The
// previous implementation waited for every chunk it had submitted, which under exactly this arrangement
// is a cycle: no chunk can run until a worker frees, no worker frees until its chunk runs. It hung.
//
// It is not a hypothetical: the cloud modelling bake runs ON a worker, and parallelising its z-slices is
// this call graph.
TEST( JobSystem, AParallelLoopInsideEveryWorkerStillFinishes )
{
    const size_t      workers = Common::JobSystem::Get().WorkerCount();
    std::atomic<int>  arrived{ 0 };
    std::atomic<int>  finished{ 0 };
    std::atomic<long> total{ 0 };

    for ( size_t w = 0; w < workers; ++w )
    {
        Common::JobSystem::Get().Submit(
             [&, workers]
             {
                 // Occupy every worker before any of them nests, so the pool really is saturated when the
                 // inner loop asks for help.
                 arrived.fetch_add( 1 );
                 while ( arrived.load() < static_cast<int>( workers ) )
                     std::this_thread::yield();

                 Common::JobSystem::Get().ParallelFor( 1024, [&]( size_t i )
                                                       { total.fetch_add( static_cast<long>( i ) ); } );
                 finished.fetch_add( 1 );
             } );
    }

    // A DEADLINE RATHER THAN AN UNBOUNDED WAIT: a returned defect must fail the suite loudly, and a sweep
    // that hangs here is a sweep nobody reads.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 60 );
    while ( finished.load() < static_cast<int>( workers ) && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();

    // AND THE PROCESS IS ENDED WHERE IT STANDS, because a deadlocked pool cannot be failed politely.
    // MEASURED, not feared: sabotaging ParallelRanges back to the old deal-then-wait shape made gtest
    // print its FAILED summary at 60 s exactly as intended — and then the binary never exited, because
    // ~JobSystem joins workers that will never wake. The sweep's `binary | grep -q FAILED` then waits for
    // a process that is gone in every sense but the one `wait` cares about, and a sweep stuck on a test is
    // worse than a red one: it reads as "still running" and nobody looks. So the verdict is printed in
    // gtest's own wording, where the sweep greps for it, and the process is killed rather than unwound.
    if ( finished.load() < static_cast<int>( workers ) )
    {
        std::printf( "%s\n%s\n",
                     "a parallel loop nested inside the pool did not finish within 60 s — the pool is "
                     "deadlocked, which is what ParallelRanges' claim-don't-deal design exists to make "
                     "impossible",
                     "[  FAILED  ] JobSystem.AParallelLoopInsideEveryWorkerStillFinishes (deadlocked)" );
        std::fflush( stdout );
        std::_Exit( 1 );
    }

    const long expected = 1023L * 1024L / 2L * static_cast<long>( workers );
    EXPECT_EQ( total.load(), expected );
}

TEST( JobSystem, WorkerCountIsPositive )
{
    EXPECT_GE( Common::JobSystem::Get().WorkerCount(), static_cast<size_t>( 1 ) );
}
