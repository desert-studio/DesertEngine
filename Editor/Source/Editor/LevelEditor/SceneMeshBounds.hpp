#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <vector>

namespace Desert::Editor
{
    // WHAT THE SCENE'S MESHES OCCUPY, in world space: the union of every placed static and skinned mesh's
    // box (Geometry::LocalBounds through the entity's world transform — the culler's extent, so the framing
    // and the culling agree on where a mesh is). EVERY mesh counts, the ground slab included: a whole-level
    // frame is UE's "Frame All", which leaves nothing out. A mesh whose asset is not parsed yet is COUNTED
    // in Missing and reported, not silently left out of the frame.
    struct SceneMeshBounds
    {
        ::Common::Math::AABB Box{ glm::vec3( Geometry::kNoBoundsSentinel ),
                                  glm::vec3( -Geometry::kNoBoundsSentinel ) };
        int                  Meshes  = 0;
        int                  Missing = 0;
    };

    // Where a PRIMITIVE's geometry comes from. The editor passes SharedPrimitiveSubmeshes — the process-wide
    // shared mesh the renderer instances every cube/sphere from (MeshECSSystem); a caller with no device
    // passes the same shape built on the CPU (PrimitiveMeshFactory::Create). Null for a type the factory
    // does not build, which the renderer does not draw either.
    using PrimitiveSubmeshes = const std::vector<Submesh>* (*)( Geometry::PrimitiveType );

    // The renderer's shared primitive mesh (PrimitiveMeshFactory::GetShared): builds its GPU buffers on
    // first use, so it needs the device.
    [[nodiscard]] const std::vector<Submesh>* SharedPrimitiveSubmeshes( Geometry::PrimitiveType type );

    // Measures @p registry's meshes in the renderer's precedence (MeshECSSystem): an edited RuntimeMesh,
    // then a PRIMITIVE (no RuntimeMesh and no handle — leaving it out framed Clouds_Showcase's six cubes as
    // "no measurable mesh"), then the asset behind the handle.
    [[nodiscard]] SceneMeshBounds MeasureSceneMeshes( entt::registry& registry, PrimitiveSubmeshes primitives );
} // namespace Desert::Editor
