#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/ECS/Entity.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The world's weather to scripts (WTH-STATE): the mechanism is the engine's (Core::WeatherState), the rules —
    /// schedules, random cycles, admin commands — are the game's Luau calling these. Under WTH-NET only the
    /// server's call is authoritative.
    struct WeatherLibrary
    {
        REFLECT( ScriptName( "Weather" ) )

        FUNCTION( ScriptCallable, ScriptName( "set" ),
                  Tooltip( "Moves a weather zone to a preset (a path under Content, e.g. "
                           "\"Weather/Rain.dweatherpreset\") over `seconds`. Ok, or false and the reason." ) )
        static Common::BoolResultStr SetWeather( ECS::Entity zone, const std::string& preset, float seconds );

        FUNCTION( ScriptCallable, ScriptName( "getScalar" ),
                  Tooltip( "A named value of the weather at a world point (a world parameter's .x, or "
                           "CloudCoverage / FogDensity / WindSpeed), or nil and the reason." ) )
        static Common::ResultStr<float> GetScalarAt( const glm::vec3& point, const std::string& name );
    };
} // namespace Desert::Libraries
