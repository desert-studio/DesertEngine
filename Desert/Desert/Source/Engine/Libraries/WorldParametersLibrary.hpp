#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The running world's parameters — UE's UKismetMaterialLibrary::Set/GetScalarParameterValue and
    /// Set/GetVectorParameterValue on a Material Parameter Collection: values of the collection the scene names
    /// (SceneSettings::WorldParameters), read by every material through `WorldParameter( "Name" )` from the next
    /// frame on. Values belong to the world the script runs in (a Play world is its own); the asset keeps the
    /// defaults.
    struct WorldParametersLibrary
    {
        REFLECT( ScriptName( "WorldParameters" ) )

        FUNCTION( ScriptCallable, ScriptName( "setScalar" ),
                  Tooltip( "Sets a scalar parameter of the world's collection. Ok, or false and the reason." ) )
        static Common::BoolResultStr SetScalar( const std::string& name, float value );

        FUNCTION( ScriptCallable, ScriptName( "setVector" ),
                  Tooltip( "Sets a vector parameter of the world's collection. Ok, or false and the reason." ) )
        static Common::BoolResultStr SetVector( const std::string& name, const glm::vec4& value );

        FUNCTION( ScriptCallable, ScriptName( "getScalar" ),
                  Tooltip( "A scalar parameter's current value, or nil and the reason." ) )
        static Common::ResultStr<float> GetScalar( const std::string& name );

        FUNCTION( ScriptCallable, ScriptName( "getVector" ),
                  Tooltip( "A vector parameter's current value, or nil and the reason." ) )
        static Common::ResultStr<glm::vec4> GetVector( const std::string& name );
    };
} // namespace Desert::Libraries
