#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

#include <cstdint>

// The emissive triangle list of one frame as the graph sees it - what a consumer (the path tracer, the surface
// cache's GI) reads. Kept apart from EmissiveLightsRenderer.hpp so a consumer does not compile the renderer.
namespace Desert::Graphic::RayTracing
{
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
