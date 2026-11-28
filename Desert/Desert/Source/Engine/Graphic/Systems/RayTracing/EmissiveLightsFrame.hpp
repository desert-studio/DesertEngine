#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

#include <cstdint>

// The emissive triangle list of one frame as the graph sees it - what a consumer (the path tracer, the surface
// cache's GI, MegaLights, the world radiance cache) reads. Kept apart from EmissiveLightsRenderer.hpp so a consumer
// does not compile the renderer. CONTRACT OF GI-TEMPORAL2: written by the architect, no package edits it.
namespace Desert::Graphic::RayTracing
{
    // The selection grid's numbers, the same as Common/EmissiveLightsSampling.glslh (kEmissiveGrid*).
    inline constexpr uint32_t kEmissiveGridHeaderRows   = 4;
    inline constexpr uint32_t kEmissiveGridSlots        = 16;
    inline constexpr uint32_t kEmissiveGridMaxCells     = 32;
    inline constexpr uint32_t kEmissiveGridClusterLevel = 4;
    inline constexpr float    kEmissiveGridRemainderMin = 0.0625f;

    // One part of the list as the graph sees it.
    struct EmissiveLightsPart
    {
        RDG::BufferRef Segments;
        RDG::BufferRef Triangles;
        RDG::BufferRef Power;
        uint32_t       TriangleCount = 0;
        uint32_t       Leaves        = 1; // the tree's level 0, a power of two
        uint32_t       Levels        = 1; // Leaves, Leaves / 2, ..., 1
    };

    // This frame's list as the graph sees it (refs valid only for that graph). A consumer binds the seven buffers
    // under Common/EmissiveLights.glslh's block names and passes StaticParams / DynamicParams (its uvec4 pair);
    // a part with TriangleCount 0 holds one empty row per buffer and a root power of 0.
    struct EmissiveLightsFrame
    {
        bool               Valid = false;
        EmissiveLightsPart Static;
        EmissiveLightsPart Dynamic;
        RDG::BufferRef     Lookup;
        // The selection grid of THIS list (EmissiveGridUB, vec4 rows, Common/EmissiveLightsSampling.glslh): valid
        // whenever Valid is; an empty list holds the header and one cell whose only weight is the remainder's.
        // A grid consumer binds it as the eighth buffer and defines EMISSIVE_LIGHTS_GRID.
        RDG::BufferRef     Grid;
        uint32_t           InstanceCount = 0; // the TLAS instances the lookup was built for
        uint64_t           Hash          = 0; // EmissiveTriangleList::Hash

        // Common/EmissiveLights.glslh's EmissiveLightsParams: x = triangles, y = leaves, z = levels, w = TLAS
        // instances of the lookup (static) / 0 (dynamic).
        [[nodiscard]] glm::uvec4 StaticParams() const
        {
            return { Static.TriangleCount, Static.Leaves, Static.Levels, InstanceCount };
        }
        [[nodiscard]] glm::uvec4 DynamicParams() const
        {
            return { Dynamic.TriangleCount, Dynamic.Leaves, Dynamic.Levels, 0u };
        }
    };
} // namespace Desert::Graphic::RayTracing
