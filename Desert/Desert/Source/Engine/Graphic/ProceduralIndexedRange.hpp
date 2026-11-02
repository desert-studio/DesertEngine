#pragma once

#include <cstdint>

namespace Desert::Graphic
{
    // One vkCmdDrawIndexed of a vertex-buffer-less indexed draw (RendererAPI::DrawProceduralIndexed): @p
    // IndexCount (> 0) uint32 indices from @p FirstIndex, each vertex's gl_VertexIndex = its index + @p
    // VertexOffset, x @p InstanceCount (> 0) instances from @p FirstInstance (gl_InstanceIndex counts from it).
    struct ProceduralIndexedRange
    {
        uint32_t IndexCount    = 0;
        uint32_t FirstIndex    = 0;
        int32_t  VertexOffset  = 0;
        uint32_t InstanceCount = 1;
        uint32_t FirstInstance = 0;
    };
} // namespace Desert::Graphic
