// THE EFFECTS CLOCK (VFX-01): fixed steps from the scene's time, seeking by replay, and a budget.
//
// Links ONE engine source (`VFX/VFXClock.cpp`) plus the header-only hash; no GPU, no scene. Asserted:
//   1. STEPS. A frame's time becomes whole fixed steps; the fraction carries; float frame deltas of
//      exactly one step give exactly one step every frame.
//   2. SCRUB. A seek forward runs floor(diff / step) steps; a seek backward is a reset plus a replay
//      from zero; the age reached by seeking is the age reached by playing.
//   3. BUDGET. A hitch is capped at MaxStepsPerTick and the excess is dropped; a long seek is spread
//      over ticks of MaxSeekStepsPerTick, reporting Seeking until it lands.
//   4. REPEAT. The same deltas give the same plans, spawn counts and random numbers, run after run.

#include <Engine/VFX/VFXClock.hpp>
#include <Engine/VFX/VFXRandom.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

using Desert::VFX::Clock;
using Desert::VFX::ClockSettings;
using Desert::VFX::SpawnAccumulator;
using Desert::VFX::TickPlan;

namespace
{
    constexpr double kStep = 1.0 / 60.0;

    struct Record
    {
        bool          Reset;
        std::uint64_t First;
        std::uint32_t Count;
        bool          Seeking;
        bool          operator==( const Record& ) const = default;
    };

    Record Of( const TickPlan& p )
    {
        return { p.Reset, p.FirstStep, p.StepCount, p.Seeking };
    }
} // namespace

// ---------------------------------------------------------------- 1. steps

TEST( VFXClock, AFloatFrameOfOneStepRunsExactlyOneStepEveryFrame )
{
    Clock       clock;
    const float frame = 1.0f / 60.0f; // what Common::Timestep carries under --play
    for ( int i = 0; i < 6000; ++i )
    {
        const TickPlan plan = clock.Advance( static_cast<double>( frame ) );
        ASSERT_EQ( plan.StepCount, 1u ) << "frame " << i;
        ASSERT_EQ( plan.FirstStep, static_cast<std::uint64_t>( i ) );
    }
    EXPECT_EQ( clock.GetStep(), 6000u );
}

TEST( VFXClock, AFasterFrameRateCarriesTheFractionAndRunsTheSameStepsOverASecond )
{
    Clock         clock;
    std::uint64_t total = 0;
    for ( int i = 0; i < 144; ++i )
    {
        const TickPlan plan = clock.Advance( 1.0 / 144.0 );
        EXPECT_LE( plan.StepCount, 1u );
        total += plan.StepCount;
    }
    EXPECT_EQ( total, 60u ) << "a second of 144 Hz frames is a second of simulation";
}

TEST( VFXClock, ZeroTimeRunsNothing )
{
    Clock clock;
    EXPECT_EQ( clock.Advance( 0.0 ).StepCount, 0u );
    EXPECT_EQ( clock.Advance( -1.0 ).StepCount, 0u );
    EXPECT_EQ( clock.GetStep(), 0u );
}

// ---------------------------------------------------------------- 2. scrub

TEST( VFXClock, ASeekForwardRunsTheWholeStepsToTheAgeWithoutAReset )
{
    Clock clock;
    clock.SeekTo( 0.5 );
    const TickPlan plan = clock.Advance( 123.0 ); // the delta is ignored while a seek is served
    EXPECT_FALSE( plan.Reset );
    EXPECT_EQ( plan.FirstStep, 0u );
    EXPECT_EQ( plan.StepCount, 30u );
    EXPECT_FALSE( plan.Seeking );
    EXPECT_EQ( clock.GetStep(), 30u );
}

TEST( VFXClock, ASeekBackwardIsAResetAndAReplayFromZero )
{
    Clock clock;
    for ( int i = 0; i < 60; ++i )
        (void)clock.Advance( kStep );
    ASSERT_EQ( clock.GetStep(), 60u );

    clock.SeekTo( 0.25 );
    const TickPlan plan = clock.Advance( kStep );
    EXPECT_TRUE( plan.Reset ) << "there is no inverse step; an earlier age is reached from zero";
    EXPECT_EQ( plan.FirstStep, 0u );
    EXPECT_EQ( plan.StepCount, 15u );
}

TEST( VFXClock, TheAgeReachedBySeekingIsTheAgeReachedByPlaying )
{
    Clock                      played;
    std::vector<std::uint64_t> playedSteps;
    for ( int i = 0; i < 45; ++i )
    {
        const TickPlan p = played.Advance( kStep );
        for ( std::uint32_t s = 0; s < p.StepCount; ++s )
            playedSteps.push_back( p.FirstStep + s );
    }

    Clock sought;
    sought.SeekTo( 45 * kStep );
    const TickPlan             p = sought.Advance( 0.0 );
    std::vector<std::uint64_t> soughtSteps;
    for ( std::uint32_t s = 0; s < p.StepCount; ++s )
        soughtSteps.push_back( p.FirstStep + s );

    EXPECT_EQ( soughtSteps, playedSteps );
    EXPECT_EQ( sought.GetStep(), played.GetStep() );
}

TEST( VFXClock, ResetIsCarriedByTheNextPlan )
{
    Clock clock;
    (void)clock.Advance( 10 * kStep );
    clock.Reset();
    const TickPlan plan = clock.Advance( kStep );
    EXPECT_TRUE( plan.Reset );
    EXPECT_EQ( plan.FirstStep, 0u );
    EXPECT_EQ( plan.StepCount, 1u );
    EXPECT_FALSE( clock.Advance( kStep ).Reset ) << "a reset is reported once";
}

// ---------------------------------------------------------------- 3. budget

TEST( VFXClock, AHitchIsCappedAndTheExcessIsDroppedNotOwed )
{
    ClockSettings settings;
    settings.MaxStepsPerTick = 4;
    Clock clock( settings );

    EXPECT_EQ( clock.Advance( 2.0 ).StepCount, 4u ) << "two seconds in one frame run four steps, not 120";
    EXPECT_EQ( clock.Advance( kStep ).StepCount, 1u ) << "and the next frame does not pay the rest back";
}

TEST( VFXClock, ALongSeekIsSpreadOverTicksAndSaysSo )
{
    ClockSettings settings;
    settings.MaxSeekStepsPerTick = 50;
    Clock clock( settings );

    clock.SeekTo( 2.0 ); // 120 steps
    std::vector<Record> plans;
    for ( int i = 0; i < 3; ++i )
        plans.push_back( Of( clock.Advance( kStep ) ) );

    const std::vector<Record> expected = {
         { false, 0, 50, true }, { false, 50, 50, true }, { false, 100, 20, false } };
    EXPECT_EQ( plans, expected );
    EXPECT_FALSE( clock.IsSeeking() );
    EXPECT_EQ( clock.Advance( kStep ).StepCount, 1u ) << "ordinary time resumes after the seek lands";
}

// ---------------------------------------------------------------- 4. repeat

TEST( VFXClock, TheSameDeltasGiveTheSamePlansSpawnsAndRandomNumbers )
{
    const auto run = []()
    {
        Clock                      clock;
        SpawnAccumulator           spawn;
        std::vector<std::uint64_t> trace;
        std::uint32_t              nextId   = 0;
        const double               deltas[] = { 0.016, 0.017, 0.033, 0.0, 0.25, 0.008, 0.008, 0.016 };
        for ( int frame = 0; frame < 400; ++frame )
        {
            if ( frame == 200 )
                clock.SeekTo( 0.5 );
            const TickPlan p = clock.Advance( deltas[frame % 8] );
            trace.push_back( ( p.FirstStep << 8 ) | p.StepCount );
            for ( std::uint32_t s = 0; s < p.StepCount; ++s )
            {
                const std::uint32_t budget = spawn.Step( 37.5, clock.GetSettings().StepSeconds );
                for ( std::uint32_t k = 0; k < budget; ++k )
                    trace.push_back( Desert::VFX::Rand4DPCG32( { 0xC0FFEEu, nextId + k, 0u, 0u } )[0] );
                nextId += budget;
            }
        }
        return trace;
    };
    EXPECT_EQ( run(), run() );
}

TEST( VFXClock, TheSpawnCarryEmitsFractionalRatesInsteadOfNever )
{
    SpawnAccumulator spawn;
    std::uint32_t    total = 0;
    for ( int i = 0; i < 60; ++i )
        total += spawn.Step( 30.0, kStep ); // half a particle per step
    EXPECT_EQ( total, 30u );
}

TEST( VFXClock, EmitterSeedsDifferByEntityAndByEmitterAndRepeat )
{
    using Desert::VFX::MakeEmitterSeed;
    std::set<std::uint32_t> seeds;
    for ( std::uint64_t uuid = 1; uuid <= 64; ++uuid )
        for ( std::uint32_t emitter = 0; emitter < 4; ++emitter )
            seeds.insert( MakeEmitterSeed( 7u, uuid, emitter ) );
    EXPECT_EQ( seeds.size(), 256u ) << "neighbouring entities or emitters must not share a sequence";
    EXPECT_EQ( MakeEmitterSeed( 7u, 0x123456789ABCull, 2u ), MakeEmitterSeed( 7u, 0x123456789ABCull, 2u ) );
    EXPECT_NE( MakeEmitterSeed( 7u, 0x100000000ull, 0u ), MakeEmitterSeed( 7u, 0x0ull, 0u ) )
         << "the UUID's high word takes part";
}
