#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/ECS/EntityValue.hpp>

namespace Desert::ECS
{
    /// Gameplay fires a field placed in the scene (UE: a Field System Actor's graph calling
    /// UFieldSystemComponent::ApplyPhysicsField on BeginPlay, a timer or an overlap):
    ///   Timer.after(1.0, function() Destruction.applyField(self) end)
    /// The entity carries the field components (Radial Impulse / Strain / Kill / Anchor Field); each fires once,
    /// at the entity's world pose, into the played scene's destruction world (ECS::FireDestructionField, the one
    /// path every caller takes). What it breaks is reported as the destructibles' OnBreak news.
    struct DestructionLibrary
    {
        REFLECT( ScriptName( "Destruction" ) )

        FUNCTION( ScriptCallable, ScriptName( "applyField" ),
                  Tooltip( "Fires every field component of the entity once into the played scene's destruction "
                           "world; returns how many bodies and pieces they acted on, -1 (logged) when it cannot "
                           "(not playing, no field component)." ) )
        static int ApplyField( Entity entity );
    };
} // namespace Desert::ECS
