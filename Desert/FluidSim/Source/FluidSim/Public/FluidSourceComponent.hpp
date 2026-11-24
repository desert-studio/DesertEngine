#pragma once

// A FLUID SOURCE (FLUID-SIM; UE reference: the Niagara Fluids "source" - an emitter shape that adds liquid to the
// grid each step). A disc at the entity's world position pouring one phase of the volume that contains it, at a
// flow rate, along a direction. A source outside every volume pours nothing and says so once.

#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::ECS
{
    struct FluidSourceData
    {
        REFLECT()

        PROPERTY( DisplayName( "Active" ), Category( "Source" ), Summary )
        bool Active = true;

        PROPERTY( DisplayName( "Phase" ), Category( "Source" ), Range( 0, 3 ),
                  Tooltip( "Index into the containing volume's Phases" ) )
        int32_t Phase = 0;

        PROPERTY( DisplayName( "Radius" ), Category( "Source" ), Length, Range( 1.0f, 1000.0f ),
                  Tooltip( "The pouring disc's radius" ) )
        float Radius = 10.0f;

        PROPERTY( DisplayName( "Flow Rate" ), Category( "Source" ), Range( 0.0f, 1000.0f ), Units( "L/s" ),
                  Tooltip( "Litres per second (1 L = 1000 cm3)" ) )
        float FlowRate = 2.0f;

        PROPERTY( DisplayName( "Direction" ), Category( "Source" ),
                  Tooltip( "World direction of the pour (normalised when read)" ) )
        glm::vec3 Direction{ 0.0f, -1.0f, 0.0f };

        PROPERTY( DisplayName( "Speed" ), Category( "Source" ), Range( 0.0f, 5000.0f ), Units( "cm/s" ) )
        float Speed = 150.0f;

        PROPERTY( DisplayName( "Duration" ), Category( "Source" ), Range( 0.0f, 3600.0f ), Units( "s" ),
                  Tooltip( "Seconds the source pours after the simulation starts; 0 = forever" ) )
        float Duration = 0.0f;
    };

    struct FluidSourceComponent
    {
        COMPONENT( Key( "FluidSource" ), Block( Data ), Run( ActorsAndUI ) )
        FluidSourceData Data;
    };
} // namespace Desert::ECS
