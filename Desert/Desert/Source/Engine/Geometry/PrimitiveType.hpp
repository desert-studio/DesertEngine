#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/Units.hpp>

#include <array>
#include <optional>

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
         PrimitiveType::Cube,  PrimitiveType::Sphere,   PrimitiveType::Pyramid,
         PrimitiveType::Plane, PrimitiveType::Cylinder, PrimitiveType::Capsule };

    // THE BOX EACH PRIMITIVE DRAWS, in its own space and in world units — and nothing for a shape that
    // draws nothing.
    //
    // ONE SOURCE, READ BY TWO SIDES THAT CANNOT SEE EACH OTHER. PrimitiveMeshFactory builds the vertices
    // and stamps THIS box on the submesh; the world partitioner (Core/Serialize/WorldPartitionRules.hpp)
    // cannot include the factory — it reaches Vulkan through DynamicMesh — and reads the same box from
    // here. Before this the partitioner restated the cube's half edge as a literal 50 beside a comment
    // naming the factory, and knew no other shape.
    //
    // NULLOPT IS AN ANSWER, not a gap: `PrimitiveMeshFactory::Create` returns nullptr for Pyramid,
    // Cylinder and Capsule, so an entity naming one draws nothing and its position is its whole extent.
    // Terrain and LightCube are built elsewhere (TerrainMeshFactory, the light gizmo) and are never the
    // `Primitive` of a mesh block.
    //
    // Unit shapes are authored on [-0.5, 0.5] and scaled to one METRE (Common/Core/Units.hpp), so a Cube
    // is 100 units a side. The Plane is a card in the XY plane with normal +Z, hence zero depth.
    [[nodiscard]] inline std::optional<Common::Math::AABB> PrimitiveBounds( PrimitiveType type )
    {
        constexpr float half = 0.5f * Common::Units::UnitsPerMetre;
        switch ( type )
        {
            case PrimitiveType::Cube:
            case PrimitiveType::Sphere:
                return Common::Math::AABB{ glm::vec3( -half ), glm::vec3( half ) };
            case PrimitiveType::Plane:
                return Common::Math::AABB{ glm::vec3( -half, -half, 0.0f ), glm::vec3( half, half, 0.0f ) };
            case PrimitiveType::Pyramid:
            case PrimitiveType::Cylinder:
            case PrimitiveType::Capsule:
            case PrimitiveType::Terrain:
            case PrimitiveType::LightCube:
            case PrimitiveType::Count:
                return std::nullopt;
        }
        return std::nullopt;
    }
} // namespace Desert::Geometry
