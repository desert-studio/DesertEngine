#pragma once

#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // What a built mesh actually holds, read straight off its submeshes. ONE definition, because the
    // numbers show up in two places that must agree: the Details panel's Mesh section and the viewport's
    // stats overlay. Two hand-rolled counts would eventually disagree, and then neither is trustworthy.
    struct MeshStats
    {
        uint32_t  Elements  = 0;
        uint64_t  Vertices  = 0;
        uint64_t  Triangles = 0; // LOD 0
        uint32_t  LODLevels = 1;
        glm::vec3 Extent    = glm::vec3( 0.0f );
        bool      HasBounds = false;
    };

    // A section's LOD 0 triangles, from its INDEX count: a vertex is shared by several triangles, so
    // VertexCount/3 undercounts an indexed mesh badly (and a mesh's Index is a whole triangle, so its
    // index-buffer size is already the count — never divide it again). The one per-section count the
    // mesh total below and the Details panel's Elements rows both read.
    [[nodiscard]] inline uint64_t SubmeshTriangles( const Submesh& submesh )
    {
        return submesh.IndexCount / 3;
    }

    inline MeshStats ComputeMeshStats( const std::vector<Submesh>& submeshes )
    {
        MeshStats stats;
        glm::vec3 mn( 1.0e30f );
        glm::vec3 mx( -1.0e30f );

        for ( const auto& sm : submeshes )
        {
            ++stats.Elements;
            stats.Vertices += sm.VertexCount;
            stats.Triangles += SubmeshTriangles( sm );
            stats.LODLevels = std::max( stats.LODLevels, static_cast<uint32_t>( sm.LODs.size() ) );

            mn = glm::min( mn, sm.BoundingBox.Min );
            mx = glm::max( mx, sm.BoundingBox.Max );
        }

        if ( mn.x <= mx.x )
        {
            stats.Extent    = mx - mn;
            stats.HasBounds = true;
        }
        return stats;
    }
} // namespace Desert::Geometry
