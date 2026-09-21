#pragma once

#include <array>

namespace Desert::Geometry
{
    enum class PrimitiveType
    {
        Cube = 0,
        Sphere,
        Pyramid,
        Plane,
        Cylinder,
        Capsule,
        Terrain,
        LightCube,

        Count
    };

    // THE NAME OF A SHAPE IS DERIVED FROM THE ENUM, NEVER TYPED BESIDE IT.
    //
    // Three hand-written lists of these names existed and ONE OF THEM WAS WRONG: the Instanced Static
    // Mesh editor offered `{ "Cube", "Sphere", "Plane", "Pyramid" }` against an enum that runs
    // Cube, Sphere, Pyramid, Plane — so picking "Plane" built a pyramid and picking "Pyramid" built a
    // plane, and the two shapes the list did not mention could not be picked at all. Nothing in the
    // frame says which enumerator a combo box meant, which is why a list positioned by hand can be
    // wrong for a year.
    //
    // NO `default:` ON PURPOSE. A ninth shape added above fails to compile here (-Wswitch, and the
    // function has no fallthrough return), so the label cannot be forgotten — the same construction
    // CloudStageName uses for the same reason.
    constexpr const char* PrimitiveTypeName( PrimitiveType type )
    {
        switch ( type )
        {
            case PrimitiveType::Cube:
                return "Cube";
            case PrimitiveType::Sphere:
                return "Sphere";
            case PrimitiveType::Pyramid:
                return "Pyramid";
            case PrimitiveType::Plane:
                return "Plane";
            case PrimitiveType::Cylinder:
                return "Cylinder";
            case PrimitiveType::Capsule:
                return "Capsule";
            case PrimitiveType::Terrain:
                return "Terrain";
            case PrimitiveType::LightCube:
                return "Light Cube";
            case PrimitiveType::Count:
                return "Count";
        }
        return "Count";
    }

    // THE SHAPES A PERSON MAY PICK. `Terrain` is built by the terrain system from a heightfield and
    // `LightCube` by the light gizmo; neither is a mesh anyone authors from a combo box, and offering
    // them would be two more knobs that move nothing. Every Details combo offering a shape reads THIS
    // array, so the order it draws and the value it stores cannot disagree.
    inline constexpr std::array<PrimitiveType, 6> kAuthorablePrimitives = {
         PrimitiveType::Cube,     PrimitiveType::Sphere,   PrimitiveType::Pyramid,
         PrimitiveType::Plane,    PrimitiveType::Cylinder, PrimitiveType::Capsule };
} // namespace Desert::Geometry
