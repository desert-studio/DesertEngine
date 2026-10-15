#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/ECS/EntityValue.hpp>
#include <Engine/Reflection/Value.hpp>

#include <glm/vec4.hpp>

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

        /// UE UNiagaraComponent::SetVariable*: this placement's value of its system's User.<name> row, written
        /// through VFX::SetComponentUserParam (the one validating writer Details and the Sequencer use).
        ///   VFX.setParam(entity, "Density", {120})          -- Float: x; Vec2/3/4: x..w; Int: whole x; Bool: x ~= 0
        FUNCTION( ScriptCallable, ScriptName( "setParam" ),
                  Tooltip( "value = {x, y, z, w} (only the row's components are kept); false (logged) when the "
                           "entity has no VFX component, its system has no such User row, or the value is not "
                           "finite." ) )
        static bool SetParam( ECS::Entity entity, const std::string& name, const glm::vec4& value );

        FUNCTION( ScriptCallable, ScriptName( "clearParam" ),
                  Tooltip( "Back to the system's default for that User row; false when it had no override." ) )
        static bool ClearParam( ECS::Entity entity, const std::string& name );
    };
} // namespace Desert::VFX
