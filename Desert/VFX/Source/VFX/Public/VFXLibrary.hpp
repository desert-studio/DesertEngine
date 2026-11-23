#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/ECS/EntityValue.hpp>
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

        /// UE UNiagaraComponent::SetVariable*: this placement's User.<name> becomes @p value from the next VFX
        /// tick (VFX.setVariable(impact, "SurfaceNormal", vector(0, 1, 0))). The value must fit the system's
        /// parameter of that name: a number for Float/Int/Bool (Bool also true/false), a vector or a list of
        /// numbers for Vec2-4. The system must be loaded, since its row gives the type.
        FUNCTION( ScriptCallable, ScriptName( "setVariable" ),
                  Tooltip( "Sets this VFX entity's own value of a User parameter of its system; false (logged) "
                           "when the entity has no VFX, the system has no such parameter or the type differs." ) )
        static bool SetVariable( ECS::Entity entity, const std::string& name, const Reflection::Value& value );
    };
} // namespace Desert::VFX
