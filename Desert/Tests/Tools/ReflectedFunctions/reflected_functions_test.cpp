// A REFLECTED FUNCTION IS CALLED THROUGH THE REGISTRY, WITH NO LANGUAGE IN THE ROOM.
//
// FUNCTION(...) is the one public layer every language binds to (SCR-API-1): DesertHeaderTool reads the
// annotation, emits a FunctionInfo whose thunk is MakeFunction<&T::F>, and a caller — Lua, a VM, an editor
// button, this test — finds it by type and name and calls FunctionInfo::Invoke with Values. Nothing below
// includes a script runtime: if a call works here, it works for every consumer, and a consumer that needs more
// than this has found a gap in the layer, not in itself.
//
// The fixture (Fixture/FunctionFixture.hpp) goes through the real tool in the Tools runner's prebuild, so the
// scanner, the template and the thunks are tested together, as the engine's own FUNCTIONs will be.

#include "Fixture/FunctionFixture.hpp"

#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>

#include <glm/vec3.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace Desert::Reflection
{
    void ForceLinkReflectedFunctionFixture(); // Generated/FunctionFixture.gen.cpp (--reflect-anchor)
}

namespace
{
    using Desert::Reflection::FieldType;
    using Desert::Reflection::FunctionInfo;
    using Desert::Reflection::Value;
    using ReflectedFunctionsFixture::Counter;

    const FunctionInfo& Function( const char* name )
    {
        Desert::Reflection::ForceLinkReflectedFunctionFixture();
        const Desert::Reflection::TypeInfo* type = Desert::Reflection::ReflectionRegistry::Get().Find( "Counter" );
        EXPECT_NE( type, nullptr ) << "the fixture type is not registered";
        const FunctionInfo* function = type != nullptr ? type->FindFunction( name ) : nullptr;
        EXPECT_NE( function, nullptr ) << "Counter::" << name << " is not reflected";
        static const FunctionInfo kMissing;
        return function != nullptr ? *function : kMissing;
    }

    TEST( ReflectedFunctions, TheRecordDescribesTheSignatureTheCompilerSees )
    {
        const FunctionInfo& add = Function( "Add" );
        EXPECT_EQ( add.Owner, "Counter" );
        ASSERT_EQ( add.Params.size(), 1u );
        EXPECT_EQ( add.Params[0].Name, "amount" );
        EXPECT_EQ( add.Params[0].Type, FieldType::Int );
        EXPECT_EQ( add.Params[0].TypeName, "int" );
        EXPECT_TRUE( add.Returns.empty() );
        EXPECT_FALSE( add.IsStatic );
        EXPECT_FALSE( add.IsConst );
        EXPECT_TRUE( add.Meta.ScriptCallable );
        EXPECT_EQ( add.Meta.Category, "Counter" );
        EXPECT_EQ( add.Meta.Tooltip, "Adds to the count." );

        const FunctionInfo& get = Function( "Get" );
        EXPECT_TRUE( get.IsConst );
        ASSERT_EQ( get.Returns.size(), 1u );
        EXPECT_EQ( get.Returns[0].Type, FieldType::Int );
        EXPECT_EQ( get.Returns[0].TypeName, "int" );

        const FunctionInfo& scale = Function( "Scale" );
        EXPECT_TRUE( scale.IsStatic );
        EXPECT_FALSE( scale.Meta.ScriptCallable );
        ASSERT_EQ( scale.Params.size(), 2u );
        EXPECT_EQ( scale.Params[1].Name, "factor" );
        EXPECT_EQ( scale.Params[1].Type, FieldType::Double );

        const FunctionInfo& greet = Function( "Greet" );
        ASSERT_EQ( greet.Params.size(), 2u );
        EXPECT_EQ( greet.Params[0].Type, FieldType::String );
        EXPECT_EQ( greet.Params[0].TypeName, "const std::string&" );
        EXPECT_EQ( greet.Params[1].Type, FieldType::Enum );
        EXPECT_EQ( Function( "Toggle" ).Params[1].Type, FieldType::UInt );

        // Declaration order is kept: a binding lists them as the header does.
        const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( "Counter" );
        ASSERT_NE( type, nullptr );
        ASSERT_EQ( type->Functions.size(), 7u );
        EXPECT_EQ( type->Functions.front().Name, "Add" );
        EXPECT_EQ( type->Functions.back().Name, "Toggle" );
        // The type's fields are untouched by its functions.
        ASSERT_EQ( type->Fields.size(), 1u );
        EXPECT_EQ( type->Fields[0].Name, "Count" );
    }

    TEST( ReflectedFunctions, AMemberCallReachesTheInstance )
    {
        Counter          counter;
        const std::array args = { Value::Int( 5 ) };
        ASSERT_TRUE( Function( "Add" ).Invoke( &counter, args.data(), args.size(), nullptr ).IsSuccess() );
        ASSERT_TRUE( Function( "Add" ).Invoke( &counter, args.data(), args.size(), nullptr ).IsSuccess() );
        EXPECT_EQ( counter.Count, 10 );

        Value result;
        ASSERT_TRUE( Function( "Get" ).Invoke( &counter, nullptr, 0, &result ).IsSuccess() );
        ASSERT_EQ( result.Type(), FieldType::Int );
        EXPECT_EQ( *result.Get<std::int64_t>(), 10 );
    }

    TEST( ReflectedFunctions, EveryKindCrossesTheBoundaryBothWays )
    {
        Value            scaled;
        const std::array scaleArgs = { Value::Float( 1.5f ), Value::Double( 3.0 ) };
        ASSERT_TRUE(
             Function( "Scale" ).Invoke( nullptr, scaleArgs.data(), scaleArgs.size(), &scaled ).IsSuccess() );
        ASSERT_EQ( scaled.Type(), FieldType::Float );
        EXPECT_FLOAT_EQ( *scaled.Get<float>(), 4.5f );

        const Counter    counter;
        Value            greeting;
        const std::array greetArgs = { Value::String( "Ann" ), Value::Enum( static_cast<std::int64_t>(
                                                                    ReflectedFunctionsFixture::Mood::Angry ) ) };
        ASSERT_TRUE(
             Function( "Greet" )
                  .Invoke( const_cast<Counter*>( &counter ), greetArgs.data(), greetArgs.size(), &greeting )
                  .IsSuccess() );
        ASSERT_EQ( greeting.Type(), FieldType::String );
        EXPECT_EQ( *greeting.Get<std::string>(), "Go away, Ann" );

        Counter seven;
        seven.Count = 7;
        Value            offset;
        const std::array offsetArgs = { Value::Vec3( { 1.0f, 2.0f, 3.0f } ) };
        ASSERT_TRUE(
             Function( "Offset" ).Invoke( &seven, offsetArgs.data(), offsetArgs.size(), &offset ).IsSuccess() );
        ASSERT_EQ( offset.Type(), FieldType::Vec3 );
        EXPECT_EQ( *offset.Get<Value::Float3>(), ( Value::Float3{ 8.0f, 9.0f, 10.0f } ) );

        Counter          toggled;
        const std::array toggleArgs = { Value::Bool( false ), Value::UInt( 4 ) };
        ASSERT_TRUE(
             Function( "Toggle" ).Invoke( &toggled, toggleArgs.data(), toggleArgs.size(), nullptr ).IsSuccess() );
        EXPECT_EQ( toggled.Count, -4 );
    }

    // Every refusal leaves the instance as it was: the check runs before the call, never halfway through it.
    TEST( ReflectedFunctions, AMismatchedCallIsRefusedNotConverted )
    {
        Counter counter;

        const std::array asFloat   = { Value::Float( 5.0f ) };
        const auto       wrongKind = Function( "Add" ).Invoke( &counter, asFloat.data(), asFloat.size(), nullptr );
        ASSERT_FALSE( wrongKind.IsSuccess() );
        EXPECT_NE( wrongKind.GetError().find( "argument 0 ('amount') is Int, got Float" ), std::string::npos )
             << wrongKind.GetError();

        const std::array two = { Value::Int( 1 ), Value::Int( 2 ) };
        EXPECT_FALSE( Function( "Add" ).Invoke( &counter, two.data(), two.size(), nullptr ).IsSuccess() );

        const std::array one = { Value::Int( 1 ) };
        EXPECT_FALSE( Function( "Add" ).Invoke( nullptr, one.data(), one.size(), nullptr ).IsSuccess() )
             << "a member function called with no instance";

        const std::array scaleArgs = { Value::Float( 1.0f ), Value::Double( 1.0 ) };
        Value            unused;
        EXPECT_FALSE(
             Function( "Scale" ).Invoke( &counter, scaleArgs.data(), scaleArgs.size(), &unused ).IsSuccess() )
             << "a static function called with an instance";
        EXPECT_FALSE( Function( "Get" ).Invoke( &counter, nullptr, 0, nullptr ).IsSuccess() )
             << "a result with nowhere to go";

        // In kind but out of range for the parameter: refused, not wrapped to -24.
        const std::array tooBig = { Value::Int( 1000 ) };
        const auto outside      = Function( "SetSmall" ).Invoke( &counter, tooBig.data(), tooBig.size(), nullptr );
        ASSERT_FALSE( outside.IsSuccess() );
        EXPECT_NE( outside.GetError().find( "outside its parameter's range" ), std::string::npos )
             << outside.GetError();

        EXPECT_EQ( counter.Count, 0 );

        const std::array fits = { Value::Int( -100 ) };
        ASSERT_TRUE( Function( "SetSmall" ).Invoke( &counter, fits.data(), fits.size(), nullptr ).IsSuccess() );
        EXPECT_EQ( counter.Count, -100 );
    }
} // namespace
