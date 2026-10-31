#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/World/Deformation/DeformationWorld.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The one argument of World.deform: `World.deform{ Stamp = "DS_CircleRim", At = vector(x, y, z),
    /// Params = { Radius = 20, Pressure = 1 } }` (a table, so a call names what it sets and leaves the rest
    /// default).
    struct DeformCall
    {
        REFLECT()

        PROPERTY( Tooltip( "The DeformStamp shader's name (its file stem), e.g. \"DS_CircleRim\"." ) )
        std::string Stamp;
        PROPERTY( Units( "cm" ), Tooltip( "World position of the contact." ) )
        glm::vec3 At = { 0.0f, 0.0f, 0.0f };
        PROPERTY( Tooltip( "The stamp's typed inputs." ) )
        World::DeformStampParams Params;
    };

    /// World.deform: the script source of the world's DeformationWorld (Engine/World/Deformation). Mechanism only:
    /// what the contact looks like is the named stamp asset's; what the surface does with it is the physical
    /// material's (DEFORM-RESPONSE).
    struct DeformationLibrary
    {
        REFLECT( ScriptName( "World" ) )

        FUNCTION( ScriptCallable, ScriptName( "deform" ),
                  Tooltip( "Presses a DeformStamp into the world's deformation field at a point: "
                           "World.deform{ Stamp = name, At = vector(x, y, z), Params = { Radius = cm, ... } }. "
                           "Ok, or false and the reason." ) )
        static Common::BoolResultStr Deform( const DeformCall& call );
    };
} // namespace Desert::Libraries
