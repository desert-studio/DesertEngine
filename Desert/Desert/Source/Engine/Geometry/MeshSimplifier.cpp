#include "MeshSimplifier.hpp"

#include <meshoptimizer.h>

#include <algorithm>

namespace Desert::Geometry
{
    SimplifyResult SimplifyMesh( const float* positions, std::size_t vertexCount,
                                 const std::vector<uint32_t>& indices, float targetRatio, float targetError,
                                 const float* attributes )
    {
        SimplifyResult out;
        if ( !positions || vertexCount == 0 || indices.size() < 3 )
        {
            out.Indices = indices;
            return out;
        }

        const float       ratio            = std::clamp( targetRatio, 0.0f, 1.0f );
        const std::size_t targetIndexCount = static_cast<std::size_t>( indices.size() * ratio ) / 3 * 3;

        std::vector<uint32_t> lod( indices.size() );
        float                 error = 0.0f;
        std::size_t           count = 0;
        if ( attributes )
        {
            // Normals live in [-1, 1], UVs mostly in [0, 1]: inside meshopt's recommended weight range,
            // normals weigh a little more - a lit silhouette shows a bent normal before a slid UV.
            static constexpr float kWeights[kSimplifyAttributeCount] = { 0.08f, 0.08f, 0.08f, 0.04f, 0.04f };
            count = meshopt_simplifyWithAttributes( lod.data(), indices.data(), indices.size(), positions,
                                                    vertexCount, sizeof( float ) * 3, attributes,
                                                    sizeof( float ) * kSimplifyAttributeCount, kWeights,
                                                    kSimplifyAttributeCount, targetIndexCount, targetError,
                                                    /*options*/ 0, &error );
        }
        else
        {
            count = meshopt_simplify( lod.data(), indices.data(), indices.size(), positions, vertexCount,
                                      sizeof( float ) * 3, targetIndexCount, targetError, /*options*/ 0, &error );
        }
        lod.resize( count );

        out.Indices = std::move( lod );
        out.Error   = error;
        return out;
    }
} // namespace Desert::Geometry
