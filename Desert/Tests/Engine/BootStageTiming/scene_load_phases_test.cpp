// SceneLoadPhases - the per-phase timing of a scene load (WP2).
//
// The instrument is only worth its lines if its numbers ADD UP: the load of a 50 179-record world was
// decomposed with it, and a phase that silently absorbed its neighbour's time (a lap that does not restart
// where the previous one ended) would have pointed at the wrong phase with a straight face.

#include <gtest/gtest.h>

#include <Engine/Core/Serialize/SceneLoadPhases.hpp>

#include <chrono>
#include <thread>

using Desert::Core::SceneLoadPhases;

namespace
{
    void BusyFor( const std::chrono::milliseconds duration )
    {
        const auto until = std::chrono::steady_clock::now() + duration;
        while ( std::chrono::steady_clock::now() < until )
            std::this_thread::yield();
    }
} // namespace

TEST( SceneLoadPhases, EachLapStartsWhereThePreviousPhaseEnded )
{
    // A long phase followed by an empty one: if the empty one were measured from construction rather than
    // from the end of the long one, it would carry the long one's time.
    SceneLoadPhases phases( "test" );
    BusyFor( std::chrono::milliseconds( 30 ) );
    phases.Lap( "long", 7 );
    phases.Lap( "empty", 0 );

    ASSERT_EQ( phases.Phases().size(), 2u );
    EXPECT_GE( phases.Phases()[0].Ms, 30.0 );
    EXPECT_LT( phases.Phases()[1].Ms, 15.0 ) << "the second lap carried the first one's time";
    EXPECT_EQ( phases.Phases()[0].Items, 7u );
}

TEST( SceneLoadPhases, TheTotalIsTheSumOfThePhasesWhetherLappedOrRun )
{
    SceneLoadPhases phases( "test" );
    phases.Run( "run", 3, [] { BusyFor( std::chrono::milliseconds( 5 ) ); } );
    const int value = phases.Run( "run with a result", 4, [] { return 42; } );
    phases.Lap( "lap", 5 );
    phases.Record( "stated", 6, 2.5 );

    EXPECT_EQ( value, 42 );
    double sum = 0.0;
    for ( const auto& phase : phases.Phases() )
        sum += phase.Ms;
    EXPECT_DOUBLE_EQ( phases.TotalMs(), sum );
    EXPECT_EQ( phases.Phases().back().Ms, 2.5 );
    phases.LogSummary();
}
