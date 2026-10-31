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
        REFLECT( ScriptStruct )

        PROPERTY( Tooltip( "The DeformStamp shader's name (its file stem), e.g. \"DS_CircleRim\"." ) )
        std::string Stamp;
        PROPERTY( Units( "cm" ), Tooltip( "World position of the contact." ) )
        glm::vec3 At = { 0.0f, 0.0f, 0.0f };
        PROPERTY( Tooltip( "The stamp's typed inputs." ) )
        World::DeformStampParams Params;
    };
} // namespace Desert::Libraries
