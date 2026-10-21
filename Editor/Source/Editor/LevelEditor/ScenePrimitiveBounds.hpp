#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <vector>

namespace Desert::Editor
{
    // WHAT THE SCENE'S DRAWN PRIMITIVES OCCUPY, in world space — UE's "Frame All" (FocusViewportOnBox over
    // the union of every primitive component's Bounds). ONE query for every kind the renderer draws:
    //   * static and skinned meshes — Geometry::LocalBounds through the entity's world transform (the
    //     culler's extent, so the framing and the culling agree on where a mesh is);
    //   * instanced meshes (foliage fields) — the mesh's local box through every instance's world matrix;
    //   * landscape tiles — the tile's square and the height range its loaded samples actually span;
    //   * effects — the `.dfx` system's fixed bounds (UE UNiagaraSystem::FixedBounds) through the entity's
    //     world transform: particles are GPU-simulated and their positions are never read back, so the
    //     authored bounds ARE the effect's extent, exactly as UE frames a Niagara component.
    // Measuring meshes alone left a particles-only scene (PART_WorldSpace) as "no measurable mesh", and the
    // shot was taken from wherever the camera happened to be, with the effect off-frame.
    // EVERY primitive counts, the ground included: a whole-level frame leaves nothing out. A primitive whose
    // asset is not read yet is COUNTED in Missing and reported, not silently left out of the frame.
    struct ScenePrimitiveBounds
    {
        ::Common::Math::AABB Box{ glm::vec3( Geometry::kNoBoundsSentinel ),
                                  glm::vec3( -Geometry::kNoBoundsSentinel ) };
        int                  Meshes    = 0; // static + skinned entities
        int                  Instances = 0; // instanced-mesh instances (foliage)
        int                  Tiles     = 0; // landscape tiles
        int                  Effects   = 0; // VFX systems
        int                  Missing   = 0;

        [[nodiscard]] int Measured() const
        {
            return Meshes + Instances + Tiles + Effects;
        }
    };

    // Where a PRIMITIVE's geometry comes from. The editor passes SharedPrimitiveSubmeshes — the process-wide
    // shared mesh the renderer instances every cube/sphere from (MeshECSSystem); a caller with no device
    // passes the same shape built on the CPU (PrimitiveMeshFactory::Create). Null for a type the factory
    // does not build, which the renderer does not draw either.
    using PrimitiveSubmeshes = const std::vector<Submesh>* (*)( Geometry::PrimitiveType );

    // The renderer's shared primitive mesh (PrimitiveMeshFactory::GetShared): builds its GPU buffers on
    // first use, so it needs the device.
    [[nodiscard]] const std::vector<Submesh>* SharedPrimitiveSubmeshes( Geometry::PrimitiveType type );

    // Measures every drawn primitive of @p registry (see ScenePrimitiveBounds). Meshes in the renderer's
    // precedence (MeshECSSystem): an edited RuntimeMesh, then a PRIMITIVE (no RuntimeMesh and no handle —
    // leaving it out framed Clouds_Showcase's six cubes as "no measurable mesh"), then the asset behind the
    // handle. Landscape tiles are exactly the ones the renderer draws (ECS::DrawableLandscapeTiles).
    [[nodiscard]] ScenePrimitiveBounds MeasureScenePrimitives( entt::registry&    registry,
                                                               PrimitiveSubmeshes primitives );
} // namespace Desert::Editor
