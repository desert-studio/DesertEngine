#pragma once

// UE MeshUtilities::GenerateStratifiedUniformHemisphereSamples, shared by the two mesh builds that port it: the
// card build (32 samples, MeshCardRepresentationUtilities.cpp) and the distance-field sign vote (49 per
// hemisphere twice, the second mirrored in Z, MeshDistanceFieldUtilities.cpp). One definition, because the two
// copies it replaces were byte-identical and MSVC's unity build compiles both builds in one translation unit.

#include <Common/Core/Math/RandomStream.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Desert::Assets
{
    /// Appends trunc(sqrt(@p numSamples))^2 jittered strata of the +Z hemisphere to @p out, each mapped by UE's
    /// UniformSampleHemisphere (phi = 2 pi u, cos theta = v), drawing two fractions per stratum from @p random.
    /// @p zSign = -1 mirrors the set into the -Z hemisphere (the distance field's second set).
    inline void AppendStratifiedUniformHemisphereSamples( const int numSamples, Common::Math::RandomStream& random,
                                                          const double zSign, std::vector<glm::dvec3>& out )
    {
        const int dim = static_cast<int>( std::sqrt( static_cast<float>( numSamples ) ) );
        for ( int x = 0; x < dim; ++x )
            for ( int y = 0; y < dim; ++y )
            {
                const float  u1       = random.GetFraction();
                const float  u2       = random.GetFraction();
                const double phi      = 2.0 * 3.14159265358979323846 * ( ( x + u1 ) / static_cast<float>( dim ) );
                const double cosTheta = ( y + u2 ) / static_cast<float>( dim );
                const double sinTheta = std::sqrt( std::max( 0.0, 1.0 - cosTheta * cosTheta ) );
                out.emplace_back( std::cos( phi ) * sinTheta, std::sin( phi ) * sinTheta, zSign * cosTheta );
            }
    }
} // namespace Desert::Assets
