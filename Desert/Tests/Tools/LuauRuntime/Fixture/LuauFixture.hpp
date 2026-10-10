#pragma once

// The LuauRuntime suite's fixture: one REFLECT() type with a field of every kind the binder converts and a
// member, a const member and a static FUNCTION(ScriptCallable), plus one FUNCTION() a script must NOT see.
// DesertHeaderTool generates Generated/LuauFixture.gen.cpp from this header in the Tools runner's prebuild.

#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>

namespace LuauRuntimeFixture
{
    struct Beacon
    {
        REFLECT()

        PROPERTY()
        float Intensity = 1.0f;

        PROPERTY()
        glm::vec3 Position{ 0.0f };

        PROPERTY()
        glm::vec4 Tint{ 1.0f };

        PROPERTY()
        std::string Label = "beacon";

        PROPERTY( ReadOnly )
        int Serial = 7;

        FUNCTION( ScriptCallable )
        void Move( const glm::vec3& by )
        {
            Position += by;
        }

        FUNCTION( ScriptCallable )
        [[nodiscard]] float Scaled( float factor ) const
        {
            return Intensity * factor;
        }

        FUNCTION( ScriptCallable )
        static int Twice( int value )
        {
            return value * 2;
        }

        FUNCTION()
        void EngineOnly()
        {
            Serial = 0;
        }
    };
} // namespace LuauRuntimeFixture
