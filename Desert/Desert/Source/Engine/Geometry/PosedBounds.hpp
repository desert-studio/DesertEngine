#pragma once

#include <glm/glm.hpp>

#include <cfloat>
#include <cstddef>
#include <iterator>
#include <vector>

namespace Desert::Geometry
{
    /// Mesh-local box of a skinned mesh AS IT IS DRAWN: every retained CPU vertex deformed by @p skin (linear
    /// blend, the same weighted sum the skinning shader takes). A skinned submesh's stored BoundingBox is in
    /// RAW-vertex space, which matches the drawn mesh only when the bind is identity; an imported rig with a
    /// scaled or offset bind draws somewhere else entirely. Picking (Scene::Raycast) and the pose thumbnail
    /// both frame what is on screen, so both measure here.
    ///
    /// Templated on the vertex range so a test can feed a stub with the same three fields
    /// (StaticVertex.Position, BoneIDs[], BoneWeights[]) without linking mesh code that needs a GPU.
    struct PosedBox
    {
        glm::vec3 Min{ FLT_MAX };
        glm::vec3 Max{ -FLT_MAX };

        /// False for a mesh with no retained vertices: there is nothing to measure, and no box stands in.
        [[nodiscard]] bool Valid() const
        {
            return Max.x >= Min.x;
        }
    };

    template <typename VertexRange>
    [[nodiscard]] PosedBox MeasurePosedVertices( const VertexRange& vertices, const std::vector<glm::mat4>& skin )
    {
        PosedBox box;
        for ( const auto& sv : vertices )
        {
            glm::vec3 pos( 0.0f );
            float     wsum = 0.0f;
            for ( size_t j = 0; j < std::size( sv.BoneWeights ); ++j )
            {
                const float w = sv.BoneWeights[j];
                if ( w <= 0.0f )
                    continue;
                const auto b = static_cast<size_t>( sv.BoneIDs[j] );
                if ( b < skin.size() )
                    pos += w * glm::vec3( skin[b] * glm::vec4( sv.StaticVertex.Position, 1.0f ) );
                wsum += w;
            }
            if ( wsum > 1e-5f )
                pos /= wsum; // weighted average (robust to weights that don't sum to exactly 1)
            else
                pos = sv.StaticVertex.Position;
            box.Min = glm::min( box.Min, pos );
            box.Max = glm::max( box.Max, pos );
        }
        return box;
    }
} // namespace Desert::Geometry
