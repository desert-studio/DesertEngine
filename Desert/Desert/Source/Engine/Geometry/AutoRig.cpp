#include "AutoRig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    float DistancePointToSegment( const glm::vec3& p, const glm::vec3& a, const glm::vec3& b )
    {
        const glm::vec3 ab   = b - a;
        const float     len2 = glm::dot( ab, ab );
        if ( len2 < 1e-12f )
            return glm::length( p - a ); // degenerate segment -> point distance
        // Project p onto the infinite line, clamp the parameter to the segment.
        const float t = glm::clamp( glm::dot( p - a, ab ) / len2, 0.0f, 1.0f );
        return glm::length( p - ( a + ab * t ) );
    }

    std::vector<SkinnedVertex> AutoSkinVertices( const std::vector<Vertex>&  vertices,
                                                 const std::vector<RigBone>& bones, float falloff,
                                                 uint32_t maxInfluences )
    {
        std::vector<SkinnedVertex> out( vertices.size() );

        const uint32_t maxInf =
             std::min<uint32_t>( maxInfluences ? maxInfluences : 1u, SkinnedVertex::MAX_BONE_INFLUENCES );

        std::vector<std::pair<float, uint32_t>> weights; // (weight, boneIndex) scratch, reused per vertex
        weights.reserve( bones.size() );

        for ( size_t vi = 0; vi < vertices.size(); ++vi )
        {
            SkinnedVertex sv;
            sv.StaticVertex = vertices[vi];

            if ( bones.empty() )
            {
                sv.BoneIDs[0]     = 0;
                sv.BoneWeights[0] = 1.0f;
                out[vi]           = sv;
                continue;
            }

            const glm::vec3 p = vertices[vi].Position;
            weights.clear();
            for ( uint32_t bi = 0; bi < bones.size(); ++bi )
            {
                const int       parent = bones[bi].Parent;
                const glm::vec3 a      = ( parent >= 0 && parent < static_cast<int>( bones.size() ) )
                                              ? bones[parent].Head
                                              : bones[bi].Head; // root: weight by the head point alone
                const float     d      = DistancePointToSegment( p, a, bones[bi].Head );
                // 1 / dist^falloff, guarded so a vertex sitting ON a bone gets a large (finite) weight.
                weights.emplace_back( 1.0f / ( std::pow( d, falloff ) + 1e-4f ), bi );
            }

            const size_t keep = std::min<size_t>( maxInf, weights.size() );
            std::partial_sort( weights.begin(), weights.begin() + keep, weights.end(),
                               []( const auto& x, const auto& y ) { return x.first > y.first; } );

            float sum = 0.0f;
            for ( size_t k = 0; k < keep; ++k )
                sum += weights[k].first;

            if ( sum > 0.0f )
            {
                for ( size_t k = 0; k < keep; ++k )
                {
                    sv.BoneIDs[k]     = weights[k].second;
                    sv.BoneWeights[k] = weights[k].first / sum;
                }
            }
            else
            {
                sv.BoneIDs[0]     = 0; // all bones infinitely far (impossible with the guard) -> bind to root
                sv.BoneWeights[0] = 1.0f;
            }

            out[vi] = sv;
        }

        return out;
    }
} // namespace Desert::Geometry
