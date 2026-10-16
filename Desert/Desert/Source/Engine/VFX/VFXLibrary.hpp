#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/Reflection/Value.hpp>

#include <string>
#include <vector>

namespace Desert::VFX
{
    /// Gameplay writes the scene's data channels (VFX-10, UE UNiagaraDataChannelWriter):
    ///   VFX.useChannel("Impacts")                        -- registers the channel ASSET named Impacts
    ///   VFX.writeChannel("Impacts", {                    -- appends entries for this frame; true when all written
    ///       { Position = vector(x, y, z), Direction = vector(x, y, z), Strength = 2.0, Team = 1 },
    ///   })
    /// A field is written by its type in the channel: Position / Direction a vector (or {x, y, z}), Color
    /// {r, g, b, a}, Float a number, Int a whole number. Entries are consumed by the scene's next VFX tick.
    struct VFXLibrary
    {
        REFLECT( ScriptName( "VFX" ) )

        FUNCTION(
             ScriptCallable, ScriptName( "useChannel" ),
             Tooltip( "Registers the data channel asset of that name (as a script names IA_Jump) on the played "
                      "scene; false (logged) when it cannot." ) )
        static bool UseChannel( const std::string& name );

        FUNCTION( ScriptCallable, ScriptName( "writeChannel" ),
                  Tooltip( "Appends one entry per record for this frame; true when every field was written." ) )
        static bool WriteChannel( const std::string& name, const std::vector<Reflection::Value::Map>& entries );
    };
} // namespace Desert::VFX
