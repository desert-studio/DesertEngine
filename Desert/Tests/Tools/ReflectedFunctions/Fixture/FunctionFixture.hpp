#pragma once

// The ReflectedFunctions suite's fixture: one REFLECT() type whose FUNCTION(...) members cover every shape the
// thunks distinguish — member / const member / static, void / value result, every Value kind, a default
// argument, a narrow integer. DesertHeaderTool generates Generated/FunctionFixture.gen.cpp from this header
// in the Tools runner's prebuild (Desert/Tests/premake5.lua), exactly as it generates the engine's.

#include <CoreReflection/ReflectionMacros.hpp>

#include <entt/entt.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace ReflectedFunctionsFixture
{
    enum class Mood : std::int32_t
    {
        Calm  = 0,
        Angry = 3,
    };

    struct Counter
    {
        REFLECT()

        PROPERTY( Category( "Counter" ) )
        int Count = 0;

        FUNCTION( ScriptCallable, Category( "Counter" ), Tooltip( "Adds to the count." ) )
        void Add( int amount )
        {
            Count += amount;
        }

        FUNCTION( ScriptCallable )
        [[nodiscard]] int Get() const
        {
            return Count;
        }

        FUNCTION()
        static float Scale( float value, double factor = 2.0 )
        {
            return value * static_cast<float>( factor );
        }

        FUNCTION( ScriptCallable )
        [[nodiscard]] std::string Greet( const std::string& name, Mood mood ) const
        {
            return ( mood == Mood::Angry ? "Go away, " : "Hello, " ) + name;
        }

        FUNCTION()
        [[nodiscard]] glm::vec3 Offset( const glm::vec3& by ) const
        {
            return by + glm::vec3( static_cast<float>( Count ) );
        }

        FUNCTION()
        void SetSmall( std::int8_t value )
        {
            Count = value;
        }

        // An EVENT(...) signature (SCR-API-3): no body, no storage — a description the tool registers as an
        // EventInfo whose kinds MakeEvent deduces from the alias.
        EVENT( Category( "Counter" ), Tooltip( "The count reached a mark." ) )
        using OnReached = void( int mark, entt::entity by, const glm::vec3& where );

        FUNCTION()
        void Toggle( bool on, std::uint16_t times )
        {
            Count = on ? static_cast<int>( times ) : -static_cast<int>( times );
        }
    };
} // namespace ReflectedFunctionsFixture
