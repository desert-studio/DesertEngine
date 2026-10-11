// THE LUAU RUNTIME: SANDBOX, WATCHDOG, BYTECODE CACHE, HOT RELOAD, CONSOLE, AND A BINDER WRITTEN ONCE.
//
// SCR-LUAU-2. Every engine object a script touches goes through one binder that reads the reflection
// (Fixture/LuauFixture.hpp through the real DesertHeaderTool): if a field or a FUNCTION(ScriptCallable) works
// here, it works for every reflected type, because nothing in the binder names a type.
//
// Each test pins a relation, not a value alone: a write in the script and the C++ field it lands in; one
// slot's global and the other slot's view of it; a reload and the slot that was already running.

#include "Fixture/LuauFixture.hpp"

#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace Desert::Reflection
{
    void ForceLinkLuauRuntimeFixture(); // Generated/LuauFixture.gen.cpp (--reflect-anchor)
}

namespace
{
    using Desert::Scripting::LuauBinding;
    using Desert::Scripting::LuauLimits;
    using Desert::Scripting::LuauRuntime;
    using Desert::Scripting::LuauSlot;
    using LuauRuntimeFixture::Beacon;

    const Desert::Reflection::TypeInfo* BeaconType()
    {
        Desert::Reflection::ForceLinkLuauRuntimeFixture();
        return Desert::Reflection::ReflectionRegistry::Get().Find( "Beacon" );
    }

    std::vector<LuauBinding> Self( Beacon& beacon )
    {
        return { LuauBinding{ .Name    = "self",
                              .Type    = BeaconType(),
                              .Resolve = [&beacon] { return &beacon; },
                              .Changed = {},
                              .Entity  = {} } };
    }

    LuauSlot MustLoad( LuauRuntime& runtime, const std::string& script, const std::string& source,
                       std::vector<LuauBinding> bindings = {} )
    {
        Common::ResultStr<LuauSlot> const slot = runtime.Load( script, source, std::move( bindings ) );
        EXPECT_TRUE( slot.IsSuccess() ) << slot.GetError();
        return slot.IsSuccess() ? slot.GetValue() : LuauSlot{ 0 };
    }

    TEST( LuauRuntime, FieldsAndMethodsReachTheCppObjectThroughReflection )
    {
        ASSERT_NE( BeaconType(), nullptr ) << "the fixture type is not registered";
        LuauRuntime runtime;
        Beacon      beacon;
        MustLoad( runtime, "beacon.luau", R"(
            self.Intensity = 2.5
            self:Move(vector.create(1, 2, 3))
            self.Label = "lit"
            self.Tint = { x = 0.5, y = 0.25, z = 1, w = 0 }
            assert(self:Scaled(2) == 5, "a const method reads the field the script wrote")
            assert(self.Position.y == 2, "a Vec3 field reads back as a vector")
            assert(self.Serial == 7)
        )",
                  Self( beacon ) );

        EXPECT_FLOAT_EQ( beacon.Intensity, 2.5f );
        EXPECT_EQ( beacon.Position, glm::vec3( 1.0f, 2.0f, 3.0f ) );
        EXPECT_EQ( beacon.Label, "lit" );
        EXPECT_EQ( beacon.Tint, glm::vec4( 0.5f, 0.25f, 1.0f, 0.0f ) );
    }

    TEST( LuauRuntime, StaticScriptCallableFunctionsLiveInTheTypesTable )
    {
        ASSERT_NE( BeaconType(), nullptr );
        LuauRuntime runtime;
        MustLoad( runtime, "static.luau", "assert(Beacon.Twice(21) == 42)" );
    }

    TEST( LuauRuntime, ReadOnlyWrongKindAndNonCallableAreRefusedWithTheReason )
    {
        LuauRuntime runtime;
        Beacon      beacon;
        auto        refused = [&]( const char* source, const char* reason )
        {
            Common::ResultStr<LuauSlot> const slot = runtime.Load( "refuse.luau", source, Self( beacon ) );
            ASSERT_FALSE( slot.IsSuccess() ) << source;
            EXPECT_NE( slot.GetError().find( reason ), std::string::npos ) << slot.GetError();
        };
        refused( "self.Serial = 3", "read-only" );
        refused( "self.Intensity = 'bright'", "expected a number, got string" );
        refused( "self:EngineOnly()", "no field or script-callable method 'EngineOnly'" );
        refused( "self:Move(1)", "expected a vector, got number" );
        refused( "self:Scaled()", "takes 1 argument(s), got 0" );
        EXPECT_EQ( beacon.Serial, 7 );
        EXPECT_FLOAT_EQ( beacon.Intensity, 1.0f );
    }

    TEST( LuauRuntime, AnObjectThatIsGoneIsAnErrorNotADanglingRead )
    {
        LuauRuntime    runtime;
        Beacon         beacon;
        bool           alive = true;
        LuauSlot const slot =
             MustLoad( runtime, "gone.luau", "function Touch() self.Intensity = 3 end",
                       { LuauBinding{ .Name    = "self",
                                      .Type    = BeaconType(),
                                      .Resolve = [&]() -> void* { return alive ? &beacon : nullptr; },
                                      .Changed = {},
                                      .Entity  = {} } } );
        alive                               = false;
        Common::BoolResultStr const touched = runtime.Call( slot, "Touch" );
        ASSERT_FALSE( touched.IsSuccess() );
        EXPECT_NE( touched.GetError().find( "is gone" ), std::string::npos ) << touched.GetError();
        EXPECT_FLOAT_EQ( beacon.Intensity, 1.0f );
    }

    TEST( LuauRuntime, TheSandboxFreezesLibrariesAndSeparatesSlots )
    {
        LuauRuntime    runtime;
        LuauSlot const a = MustLoad( runtime, "a.luau", R"(
            Shared = 1
            assert(os.execute == nil and io == nil and loadstring == nil and dofile == nil and loadfile == nil)
            function Break() string.upper = nil end
        )" );
        LuauSlot const b = MustLoad( runtime, "b.luau", R"(
            function Check() assert(Shared == nil, "a global of slot a leaked into slot b"); assert(string.upper("x") == "X") end
        )" );

        Common::BoolResultStr const broke = runtime.Call( a, "Break" );
        ASSERT_FALSE( broke.IsSuccess() ) << "a script wrote into the shared string library";
        EXPECT_NE( broke.GetError().find( "readonly" ), std::string::npos ) << broke.GetError();
        Common::BoolResultStr const checked = runtime.Call( b, "Check" );
        EXPECT_TRUE( checked.IsSuccess() ) << checked.GetError();
    }

    TEST( LuauRuntime, TheWatchdogStopsAnEndlessLoopAndTheRuntimeLivesOn )
    {
        LuauRuntime    runtime( LuauLimits{ .CallBudget = std::chrono::milliseconds( 50 ) } );
        LuauSlot const slot = MustLoad( runtime, "loop.luau", R"(
            Ticks = 0
            function OnUpdate() while true do end end
            function Tick() Ticks += 1; assert(Ticks == 1) end
        )" );

        const auto                  start  = std::chrono::steady_clock::now();
        Common::BoolResultStr const looped = runtime.Call( slot, "OnUpdate" );
        ASSERT_FALSE( looped.IsSuccess() );
        EXPECT_NE( looped.GetError().find( "watchdog" ), std::string::npos ) << looped.GetError();
        EXPECT_LT( std::chrono::steady_clock::now() - start, std::chrono::seconds( 5 ) );

        Common::BoolResultStr const ticked = runtime.Call( slot, "Tick" );
        EXPECT_TRUE( ticked.IsSuccess() ) << ticked.GetError();
    }

    TEST( LuauRuntime, AScriptPastItsMemoryIsStoppedAndAccountedToItself )
    {
        LuauRuntime    runtime( LuauLimits{ .ScriptMemoryBytes = std::size_t{ 1 } << 20U } );
        LuauSlot const modest = MustLoad( runtime, "small.luau", "Kept = {}" );
        LuauSlot const hog    = MustLoad( runtime, "hog.luau", R"(
            function Grow() Hoard = {}; for i = 1, 1e7 do Hoard[i] = tostring(i) end end
        )" );
        (void)modest;

        Common::BoolResultStr const grown = runtime.Call( hog, "Grow" );
        ASSERT_FALSE( grown.IsSuccess() );
        EXPECT_NE( grown.GetError().find( "holds more than" ), std::string::npos ) << grown.GetError();
        EXPECT_GT( runtime.ScriptMemory( "hog.luau" ), runtime.ScriptMemory( "small.luau" ) );
    }

    TEST( LuauRuntime, OneScriptCompilesOnceForAllItsSlots )
    {
        LuauRuntime       runtime;
        const std::string source = "Value = 1";
        for ( int i = 0; i < 3; ++i )
            MustLoad( runtime, "many.luau", source );
        EXPECT_EQ( runtime.CompileCount(), 1u );
        MustLoad( runtime, "other.luau", source );
        EXPECT_EQ( runtime.CompileCount(), 2u ) << "the cache is keyed by the script, not only by its text";
    }

    TEST( LuauRuntime, HotReloadRerunsRunningSlotsAndABrokenReloadKeepsTheOldCode )
    {
        LuauRuntime    runtime;
        Beacon         beacon;
        LuauSlot const slot =
             MustLoad( runtime, "reload.luau", "function Apply() self.Intensity = 1.5 end", Self( beacon ) );

        Common::BoolResultStr const reloaded =
             runtime.Reload( "reload.luau", "function Apply() self.Intensity = 4 end" );
        ASSERT_TRUE( reloaded.IsSuccess() ) << reloaded.GetError();
        ASSERT_TRUE( runtime.Call( slot, "Apply" ).IsSuccess() );
        EXPECT_FLOAT_EQ( beacon.Intensity, 4.0f ) << "the running slot still ran the old code";

        Common::BoolResultStr const broken =
             runtime.Reload( "reload.luau", "function Apply( self.Intensity = 9 end" );
        EXPECT_FALSE( broken.IsSuccess() );
        beacon.Intensity = 0.0f;
        ASSERT_TRUE( runtime.Call( slot, "Apply" ).IsSuccess() );
        EXPECT_FLOAT_EQ( beacon.Intensity, 4.0f ) << "a reload that did not compile replaced the working code";
    }

    TEST( LuauRuntime, TheConsoleEvaluatesExpressionsAndKeepsItsGlobals )
    {
        LuauRuntime runtime;
        std::string output;
        ASSERT_TRUE( runtime.Eval( "1 + 2", output ).IsSuccess() );
        EXPECT_EQ( output, "3\n" );

        output.clear();
        ASSERT_TRUE( runtime.Eval( "answer = 21", output ).IsSuccess() );
        ASSERT_TRUE( runtime.Eval( "answer * 2", output ).IsSuccess() );
        EXPECT_EQ( output, "42\n" );

        output.clear();
        ASSERT_TRUE( runtime.Eval( "print('hi', 1)", output ).IsSuccess() );
        EXPECT_EQ( output, "hi\t1\n" );

        output.clear();
        EXPECT_FALSE( runtime.Eval( "error('boom')", output ).IsSuccess() );
    }

    TEST( LuauRuntime, EventsTakeValuesAndAbsentFunctionsAreNamed )
    {
        LuauRuntime    runtime;
        Beacon         beacon;
        LuauSlot const slot =
             MustLoad( runtime, "event.luau",
                       "function OnUpdate(dt, tag) self.Intensity = dt; self.Label = tag end", Self( beacon ) );
        EXPECT_TRUE( runtime.Defines( slot, "OnUpdate" ) );
        EXPECT_FALSE( runtime.Defines( slot, "OnStart" ) );

        const std::array            args   = { Desert::Reflection::Value::Float( 0.25f ),
                                               Desert::Reflection::Value::String( "tick" ) };
        Common::BoolResultStr const called = runtime.Call( slot, "OnUpdate", args );
        ASSERT_TRUE( called.IsSuccess() ) << called.GetError();
        EXPECT_FLOAT_EQ( beacon.Intensity, 0.25f );
        EXPECT_EQ( beacon.Label, "tick" );

        Common::BoolResultStr const absent = runtime.Call( slot, "OnStart" );
        ASSERT_FALSE( absent.IsSuccess() );
        EXPECT_NE( absent.GetError().find( "OnStart" ), std::string::npos );
    }
} // namespace
