// LuauComponents — a REAL component on a REAL entity reached from a Luau script through the one component table
// (ECS::ReflectedComponents) by entity:component(key), via Scripting::EntityBinding / ComponentBinding.
//
// Relations pinned: what the script reads is the live component (a vector field as a 1-based list); what it
// writes lands in that component and ends in its on_update; an absent component is nil, an unknown key is an
// error (a typo never reads as "absent"); a removed component / destroyed entity is an error naming it, never a
// dangling read; a write of the wrong kind writes nothing and fires nothing.

#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    namespace ECS       = Desert::ECS;
    namespace Scripting = Desert::Scripting;

    int  gUpdates = 0;
    void CountUpdate( entt::registry&, entt::entity )
    {
        ++gUpdates;
    }

    struct World
    {
        entt::registry         Registry;
        entt::entity           Entity = Registry.create();
        Scripting::LuauRuntime Runtime;

        World()
        {
            gUpdates = 0;
            Registry.on_update<ECS::DestructibleComponent>().connect<&CountUpdate>();
        }

        Scripting::LuauSlot Load( const std::string& source )
        {
            Scripting::LuauBinding const self = Scripting::EntityBinding(
                 "self", [this] { return Scripting::LuauEntityRef{ &Registry, Entity }; } );
            auto slot = Runtime.Load( "LuauComponents", source, { self } );
            EXPECT_TRUE( slot.IsSuccess() ) << ( slot.IsSuccess() ? "" : slot.GetError() );
            return slot.IsSuccess() ? slot.GetValue() : Scripting::LuauSlot{};
        }

        std::vector<float>& Thresholds()
        {
            return Registry.get<ECS::DestructibleComponent>( Entity ).Data.DamageThreshold;
        }
    };
} // namespace

TEST( LuauComponents, AScriptReadsAndWritesTheLiveComponentAndFiresOnUpdate )
{
    World w;
    w.Registry.emplace<ECS::DestructibleComponent>( w.Entity ).Data.DamageThreshold = { 10.0f, 20.0f, 30.0f };
    const Scripting::LuauSlot slot                                                  = w.Load( R"(
        function Run()
            local d = self:component("Destructible")
            assert(#d.DamageThreshold == 3, "length")
            assert(d.DamageThreshold[2] == 20, "element")
            assert(d.DamageThreshold[4] == nil, "past the end")
            d.DamageThreshold[1] = 15
            d.DamageThreshold[4] = 40
            d.DamageThreshold[4] = nil
        end
    )" );
    ASSERT_TRUE( w.Runtime.Call( slot, "Run" ).IsSuccess() );
    EXPECT_EQ( w.Thresholds(), ( std::vector<float>{ 15.0f, 20.0f, 30.0f } ) );
    EXPECT_EQ( gUpdates, 3 ); // one per landed write
}

TEST( LuauComponents, AWrongKindWritesNothingAndFiresNothing )
{
    World w;
    w.Registry.emplace<ECS::DestructibleComponent>( w.Entity ).Data.DamageThreshold = { 10.0f };
    const Scripting::LuauSlot slot                                                  = w.Load( R"(
        function Element() self:component("Destructible").DamageThreshold[1] = "x" end
        function Whole() self:component("Destructible").DamageThreshold = { 1, "x" } end
        function Hole() self:component("Destructible").DamageThreshold[5] = 1 end
    )" );
    EXPECT_FALSE( w.Runtime.Call( slot, "Element" ).IsSuccess() );
    EXPECT_FALSE( w.Runtime.Call( slot, "Whole" ).IsSuccess() );
    EXPECT_FALSE( w.Runtime.Call( slot, "Hole" ).IsSuccess() );
    EXPECT_EQ( w.Thresholds(), ( std::vector<float>{ 10.0f } ) );
    EXPECT_EQ( gUpdates, 0 );
}

TEST( LuauComponents, AbsentIsNilUnknownKeyIsAnErrorGoneIsAnError )
{
    World                     w;
    const Scripting::LuauSlot slot = w.Load( R"(
        function Absent() assert(self:component("Destructible") == nil) end
        function Typo() return self:component("Destructable") end
        function Hold() held = self:component("Destructible") end
        function Read() return #held.DamageThreshold end
    )" );
    EXPECT_TRUE( w.Runtime.Call( slot, "Absent" ).IsSuccess() );
    EXPECT_FALSE( w.Runtime.Call( slot, "Typo" ).IsSuccess() );

    w.Registry.emplace<ECS::DestructibleComponent>( w.Entity );
    ASSERT_TRUE( w.Runtime.Call( slot, "Hold" ).IsSuccess() );
    EXPECT_TRUE( w.Runtime.Call( slot, "Read" ).IsSuccess() );
    w.Registry.remove<ECS::DestructibleComponent>( w.Entity );
    const auto gone = w.Runtime.Call( slot, "Read" );
    ASSERT_FALSE( gone.IsSuccess() );
    EXPECT_NE( gone.GetError().find( "gone" ), std::string::npos ) << gone.GetError();
}
