// VFX-06. The CPU half of the emitter stack (Engine/VFX/VFXEmitterSpawn): the EmitterUpdate group read into a
// spawn plan, and the births it gives per fixed step. Pinned here:
//
//   - a rate over N steps bears floor(N * dt * rate), the fraction carried, whatever the step;
//   - a burst fires exactly once in every loop, in the step whose window holds its time, never between loops;
//   - the lifecycle clips both: nothing before the Delay, nothing after the last loop of Once / Multiple;
//   - a plan is read from the stack: rates add, a User.* binding reads the parameter's default, and every
//     refusal names the emitter, module and input.

#include <gtest/gtest.h>

#include <Engine/VFX/VFXEmitterSpawn.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    namespace S   = Desert::Assets::Serialization;
    namespace VFX = Desert::VFX;

    constexpr double kStep = 1.0 / 60.0;

    S::VFXModuleInput Value( std::string name, S::VFXValueType type, float value )
    {
        S::VFXModuleInput in;
        in.Name   = std::move( name );
        in.Type   = type;
        in.Source = S::VFXInputSource::Value;
        in.Value  = glm::vec4( value, 0, 0, 0 );
        return in;
    }

    S::VFXModuleUse Rate( float perSecond )
    {
        return { std::string( VFX::kVFXSpawnRateModule ),
                 true,
                 { Value( "SpawnRate", S::VFXValueType::Float, perSecond ) } };
    }

    S::VFXModuleUse Burst( float count, float time )
    {
        return { std::string( VFX::kVFXSpawnBurstModule ),
                 true,
                 { Value( "SpawnCount", S::VFXValueType::Int, count ),
                   Value( "SpawnTime", S::VFXValueType::Float, time ) } };
    }

    S::VFXSystemData System( std::vector<S::VFXModuleUse> emitterUpdate, S::VFXEmitterLifecycle lifecycle = {} )
    {
        S::VFXSystemData  system;
        S::VFXEmitterData emitter;
        emitter.Name                = "Sparks";
        emitter.Lifecycle           = lifecycle;
        emitter.Stack.EmitterUpdate = std::move( emitterUpdate );
        system.Emitters.push_back( emitter );
        return system;
    }

    VFX::VFXSpawnPlan Plan( const S::VFXSystemData& system )
    {
        auto plan = VFX::CompileSpawnPlan( system, 0 );
        EXPECT_TRUE( plan.IsSuccess() ) << plan.GetError();
        return plan.IsSuccess() ? plan.ExtractValue() : VFX::VFXSpawnPlan{};
    }

    std::string Refusal( const S::VFXSystemData& system )
    {
        const auto plan = VFX::CompileSpawnPlan( system, 0 );
        EXPECT_FALSE( plan.IsSuccess() );
        return plan.IsSuccess() ? std::string{} : plan.GetError();
    }

    // The births of each of @p steps steps.
    std::vector<std::uint32_t> Births( const VFX::VFXSpawnPlan& plan, int steps, double step = kStep )
    {
        VFX::SpawnState            state;
        std::vector<std::uint32_t> born;
        for ( int s = 0; s < steps; ++s )
            born.push_back( state.Step( plan, step ) );
        return born;
    }

    std::uint64_t Sum( const std::vector<std::uint32_t>& born )
    {
        std::uint64_t total = 0;
        for ( const std::uint32_t b : born )
            total += b;
        return total;
    }
} // namespace

// Times and counts below stay off step boundaries: a step's window is a sum of doubles, so a burst or a rate total
// that lands exactly on a boundary may fall to either side by an ulp — what is pinned is the count, not the ulp.

TEST( VFXEmitterSpawn, ARateOverNStepsBearsTheFloorOfItsProduct )
{
    // 25/s at 60 steps/s is 5/12 of a particle a step: the carry turns it into whole births.
    const auto plan = Plan( System( { Rate( 25.0f ) } ) );
    for ( const int n : { 1, 2, 3, 5, 13, 59, 61, 601 } )
        EXPECT_EQ( Sum( Births( plan, n ) ), static_cast<std::uint64_t>( n * 25 / 60 ) ) << n << " steps";

    // Every step bears 0 or 1 at this rate — the births are spread, not bunched.
    for ( const std::uint32_t b : Births( plan, 120 ) )
        EXPECT_LE( b, 1u );
}

TEST( VFXEmitterSpawn, RatesAddAndADisabledModuleBearsNothing )
{
    auto system = System( { Rate( 35.0f ), Rate( 90.0f ), Rate( 1000.0f ) } );
    system.Emitters[0].Stack.EmitterUpdate[2].Enabled = false;
    const auto plan = Plan( system );
    EXPECT_DOUBLE_EQ( plan.Rate, 125.0 );
    EXPECT_EQ( Sum( Births( plan, 61 ) ), 127u ); // floor(61 / 60 * 125) = floor(127.08)
}

TEST( VFXEmitterSpawn, ABurstFiresOncePerLoopInTheStepThatHoldsItsTime )
{
    S::VFXEmitterLifecycle life;
    life.LoopDuration = 0.5f;
    const auto plan   = Plan( System( { Burst( 7, 0.26f ) }, life ) );

    // Four loops of 30 steps: one burst each, in step 15 of the loop (0.26 s lies in [15/60, 16/60)).
    const auto born = Births( plan, 120 );
    EXPECT_EQ( Sum( born ), 28u );
    for ( int s = 0; s < 120; ++s )
        EXPECT_EQ( born[s], s % 30 == 15 ? 7u : 0u ) << "step " << s;

    // A burst at the loop's start fires in the loop's first step, and a step longer than the loop still fires it
    // once per loop it covers.
    const auto atStart = Plan( System( { Burst( 3, 0.0f ) }, life ) );
    EXPECT_EQ( Births( atStart, 1 ), ( std::vector<std::uint32_t>{ 3u } ) );
    EXPECT_EQ( Births( atStart, 2, 1.25 ), ( std::vector<std::uint32_t>{ 9u, 6u } ) ); // loops 0,1,2 then 3,4
}

TEST( VFXEmitterSpawn, TheLifecycleClipsRateAndBursts )
{
    // Once, 1.01 s, after a 0.505 s delay (inside step 30): nothing before step 30 nor after step 90 (the loop ends
    // at 1.515 s, inside step 90); the burst at 0 fires in step 30; the rate bears floor(60 * 1.01) = 60.
    S::VFXEmitterLifecycle once;
    once.Delay        = 0.505f;
    once.LoopDuration = 1.01f;
    once.Loop         = S::VFXLoopBehavior::Once;
    const auto born   = Births( Plan( System( { Rate( 60.0f ), Burst( 10, 0.0f ) }, once ) ), 300 );
    EXPECT_EQ( Sum( born ), 70u );
    for ( int s = 0; s < 300; ++s )
        if ( s < 30 || s > 90 )
            EXPECT_EQ( born[s], 0u ) << "step " << s;
    EXPECT_GE( born[30], 10u );

    // Multiple, three loops of 0.5 s: three bursts and floor(45 * 1.5) = 67 rate births, then silence.
    S::VFXEmitterLifecycle three;
    three.LoopDuration = 0.5f;
    three.Loop         = S::VFXLoopBehavior::Multiple;
    three.LoopCount    = 3;
    const auto plan    = Plan( System( { Rate( 45.0f ), Burst( 4, 0.11f ) }, three ) );
    EXPECT_EQ( Sum( Births( plan, 90 ) ), 67u + 12u );
    EXPECT_EQ( Sum( Births( plan, 600 ) ), 67u + 12u );

    // Infinite never stops: 10 s of 0.5 s loops is twenty bursts.
    S::VFXEmitterLifecycle forever;
    forever.LoopDuration = 0.5f;
    EXPECT_EQ( Sum( Births( Plan( System( { Burst( 1, 0.01f ) }, forever ) ), 600 ) ), 20u );
}

TEST( VFXEmitterSpawn, AUserBindingReadsTheParameterDefault )
{
    auto system = System( { Rate( 0.0f ) } );
    system.UserParams.push_back( { "Density", S::VFXValueType::Float, glm::vec4( 42.0f, 0, 0, 0 ) } );
    auto& in  = system.Emitters[0].Stack.EmitterUpdate[0].Inputs[0];
    in.Source = S::VFXInputSource::Binding;
    in.Value.reset();
    in.Binding = "User.Density";
    EXPECT_DOUBLE_EQ( Plan( system ).Rate, 42.0 );
}

TEST( VFXEmitterSpawn, RefusalsNameWhatIsWrong )
{
    auto gpu                                      = System( { Rate( 1.0f ) } );
    gpu.Emitters[0].Stack.EmitterUpdate[0].Module = "engine:Gravity";
    EXPECT_NE( Refusal( gpu ).find( "engine:SpawnRate" ), std::string::npos );

    auto missing = System( { Burst( 1, 0.0f ) } );
    missing.Emitters[0].Stack.EmitterUpdate[0].Inputs.pop_back();
    EXPECT_NE( Refusal( missing ).find( "'SpawnTime'" ), std::string::npos );

    auto mistyped                                              = System( { Burst( 1, 0.0f ) } );
    mistyped.Emitters[0].Stack.EmitterUpdate[0].Inputs[0].Type = S::VFXValueType::Float;
    EXPECT_NE( Refusal( mistyped ).find( "'SpawnCount'" ), std::string::npos );

    auto  random = System( { Rate( 1.0f ) } );
    auto& in     = random.Emitters[0].Stack.EmitterUpdate[0].Inputs[0];
    in.Source    = S::VFXInputSource::Random;
    in.Random    = S::VFXRandomRange{};
    EXPECT_NE( Refusal( random ).find( "random range" ), std::string::npos );

    S::VFXEmitterLifecycle life;
    life.LoopDuration      = 1.0f;
    const std::string late = Refusal( System( { Burst( 1, 1.0f ) }, life ) );
    EXPECT_NE( late.find( "never fire" ), std::string::npos ) << late;
    EXPECT_NE( late.find( "emitter 'Sparks'" ), std::string::npos ) << late;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
