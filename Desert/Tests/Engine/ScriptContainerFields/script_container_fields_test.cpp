// ScriptContainerFields — a std::vector field of a reflected component, reached from a REAL Luau script through
// self:component(...) (Scripting::RegisterEntityCoreBindings + RegisterReflectionBindings in a real sol2 state).
//
// Relations pinned: what the script reads is the live vector of the component on the entity (length and
// elements, 1-based); what it writes lands in that vector (element, append at n+1, remove by nil at n, whole
// table) and ends in the component's on_update; a write of the wrong kind / a fractional integer / an index
// outside 1..n+1 leaves the vector as it was, and a table with one bad element writes none; a vector of asset
// handles reads and writes each handle's 64-bit id, exactly as a single handle field does.

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>
#include <Engine/Scripting/Internal/ScriptRuntime.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    namespace ECS = Desert::ECS;

    int  gDestructibleUpdates = 0;
    void CountDestructibleUpdate( entt::registry&, entt::entity )
    {
        ++gDestructibleUpdates;
    }

    struct ScriptedEntity
    {
        Desert::Core::Scene                   Scene;
        Desert::Scripting::ScriptEngine::Impl Impl;
        entt::entity                          Handle = entt::null;

        ScriptedEntity()
        {
            Impl.Scene = &Scene;
            Impl.Lua.open_libraries( sol::lib::base );
            Desert::Scripting::RegisterEntityCoreBindings( Impl );
            Desert::Scripting::RegisterReflectionBindings( Impl );
            Handle           = Scene.GetRegistry().create();
            Impl.Lua["self"] = Impl.MakeEntity( Handle );
        }

        void Run( const std::string& code )
        {
            const auto r = Impl.Lua.safe_script( code, sol::script_pass_on_error );
            if ( !r.valid() )
            {
                const sol::error err = r; // sol2's documented conversion (MSVC rejects sol::error( r ))
                FAIL() << err.what();
            }
        }

        ECS::DestructibleData& Destructible()
        {
            return Scene.GetRegistry().get<ECS::DestructibleComponent>( Handle ).Data;
        }
    };
} // namespace

TEST( ScriptContainerFields, AScriptReadsTheLiveVectorOneBased )
{
    ScriptedEntity e;
    e.Scene.GetRegistry().emplace<ECS::DestructibleComponent>( e.Handle ).Data.DamageThreshold = { 10.0f, 20.0f,
                                                                                                   30.5f };
    e.Run( R"(
        local d = self:component("Destructible")
        count = #d.DamageThreshold
        first = d.DamageThreshold[1]
        last  = d.DamageThreshold[3]
        past  = d.DamageThreshold[4]
        zero  = d.DamageThreshold[0]
    )" );
    auto& L = e.Impl.Lua;
    EXPECT_EQ( L.get<int>( "count" ), 3 );
    EXPECT_EQ( L.get<float>( "first" ), 10.0f );
    EXPECT_EQ( L.get<float>( "last" ), 30.5f );
    EXPECT_FALSE( L.get<sol::object>( "past" ).valid() );
    EXPECT_FALSE( L.get<sol::object>( "zero" ).valid() );
}

TEST( ScriptContainerFields, ElementWriteAppendAndRemoveLandInTheComponentAndFireOnUpdate )
{
    ScriptedEntity e;
    e.Scene.GetRegistry().emplace<ECS::DestructibleComponent>( e.Handle ).Data.AnchoredNodes = { 1, 2, 3 };
    e.Scene.GetRegistry().on_update<ECS::DestructibleComponent>().connect<&CountDestructibleUpdate>();
    gDestructibleUpdates = 0;
    e.Run( R"(
        local d = self:component("Destructible")
        d.AnchoredNodes[2] = 7      -- element
        d.AnchoredNodes[4] = 9      -- n + 1 appends
        d.AnchoredNodes[1] = nil    -- nil not at the last index: nothing
        d.AnchoredNodes[4] = nil    -- nil at the last index removes it
        d.AnchoredNodes[3] = nil
    )" );
    EXPECT_EQ( e.Destructible().AnchoredNodes, ( std::vector<int32_t>{ 1, 7 } ) );
    EXPECT_EQ( gDestructibleUpdates, 4 ); // the four writes that landed; the refused nil fired nothing
}

TEST( ScriptContainerFields, AWholeTableReplacesTheVector )
{
    ScriptedEntity e;
    e.Scene.GetRegistry().emplace<ECS::DestructibleComponent>( e.Handle ).Data.AnchoredNodes = { 5 };
    e.Run( R"(
        local d = self:component("Destructible")
        d.AnchoredNodes   = { 0, 4, 7, 11 }
        d.DamageThreshold = { 1.5, 2.5 }
    )" );
    EXPECT_EQ( e.Destructible().AnchoredNodes, ( std::vector<int32_t>{ 0, 4, 7, 11 } ) );
    EXPECT_EQ( e.Destructible().DamageThreshold, ( std::vector<float>{ 1.5f, 2.5f } ) );
}

TEST( ScriptContainerFields, ARefusedWriteLeavesTheVectorAsItWas )
{
    ScriptedEntity e;
    e.Scene.GetRegistry().emplace<ECS::DestructibleComponent>( e.Handle ).Data.AnchoredNodes = { 1, 2, 3 };
    e.Scene.GetRegistry().on_update<ECS::DestructibleComponent>().connect<&CountDestructibleUpdate>();
    gDestructibleUpdates = 0;
    e.Run( R"(
        local d = self:component("Destructible")
        d.AnchoredNodes[1] = "four"           -- wrong kind
        d.AnchoredNodes[2] = 2.5              -- an integer element takes a whole number only
        d.AnchoredNodes[3] = 3000000000       -- outside int32
        d.AnchoredNodes[5] = 5                -- past n + 1
        d.AnchoredNodes    = { 9, 8, "x" }    -- one bad element: none written
        d.AnchoredNodes    = { 9, 4294967296 } -- one out of range: none written, the old ones back
        d.AnchoredNodes    = 12               -- not a table
    )" );
    EXPECT_EQ( e.Destructible().AnchoredNodes, ( std::vector<int32_t>{ 1, 2, 3 } ) );
    EXPECT_EQ( gDestructibleUpdates, 0 );
}

TEST( ScriptContainerFields, AVectorOfAssetHandlesTravelsAsTheHandlesBits )
{
    ScriptedEntity          e;
    constexpr std::uint64_t kHandle = 0x123456789ABull; // below 2^53: a Luau number holds it exactly
    e.Scene.GetRegistry().emplace<ECS::ProceduralFoliageComponent>( e.Handle ).Data.FoliageTypes = {
         Desert::Assets::AssetHandle( kHandle ) };
    e.Run( R"(
        local f = self:component("ProceduralFoliage")
        read = f.FoliageTypes[1]
        f.FoliageTypes[2] = read + 1
    )" );
    EXPECT_EQ( e.Impl.Lua.get<double>( "read" ), static_cast<double>( kHandle ) );
    const auto& types = e.Scene.GetRegistry().get<ECS::ProceduralFoliageComponent>( e.Handle ).Data.FoliageTypes;
    ASSERT_EQ( types.size(), 2u );
    EXPECT_EQ( static_cast<std::uint64_t>( types[1] ), kHandle + 1 );
}
