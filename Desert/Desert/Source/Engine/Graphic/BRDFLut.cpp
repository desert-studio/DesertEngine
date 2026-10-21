#include "BRDFLut.hpp"

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Graphic
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        struct Vec3
        {
            float x, y, z;
        };

        // Van der Corput radical inverse -> Hammersley low-discrepancy sequence.
        float RadicalInverseVdC( uint32_t bits )
        {
            bits = ( bits << 16u ) | ( bits >> 16u );
            bits = ( ( bits & 0x55555555u ) << 1u ) | ( ( bits & 0xAAAAAAAAu ) >> 1u );
            bits = ( ( bits & 0x33333333u ) << 2u ) | ( ( bits & 0xCCCCCCCCu ) >> 2u );
            bits = ( ( bits & 0x0F0F0F0Fu ) << 4u ) | ( ( bits & 0xF0F0F0F0u ) >> 4u );
            bits = ( ( bits & 0x00FF00FFu ) << 8u ) | ( ( bits & 0xFF00FF00u ) >> 8u );
            return static_cast<float>( bits ) * 2.3283064365386963e-10f; // / 0x100000000
        }

        // GGX importance sample around N = (0,0,1).
        Vec3 ImportanceSampleGGX( float u1, float u2, float roughness )
        {
            const float a        = roughness * roughness;
            const float phi      = 2.0f * kPi * u1;
            const float cosTheta = std::sqrt( ( 1.0f - u2 ) / ( 1.0f + ( a * a - 1.0f ) * u2 ) );
            const float sinTheta = std::sqrt( std::max( 0.0f, 1.0f - cosTheta * cosTheta ) );
            return { sinTheta * std::cos( phi ), sinTheta * std::sin( phi ), cosTheta };
        }

        // Smith geometry term with the IBL k remapping (k = alpha / 2, alpha = roughness^2).
        float GeometrySchlickGGX( float NdotX, float k )
        {
            return NdotX / ( NdotX * ( 1.0f - k ) + k );
        }

        // One texel: integrate (scale, bias) for F0 over the GGX-importance-sampled hemisphere.
        void IntegrateBRDF( float NdotV, float roughness, uint32_t samples, float& outScale, float& outBias )
        {
            const Vec3 V = { std::sqrt( std::max( 0.0f, 1.0f - NdotV * NdotV ) ), 0.0f, NdotV };

            // Karis 2013's IBL remapping k = alpha / 2 with alpha = roughness^2 (LearnOpenGL's k = roughness^2 /
            // 2, UE's preintegrated EnvBRDF). It was alpha^2 / 2 = roughness^4 / 2: G barely attenuated a grazing
            // view, and the LUT's scale + bias reached 1.44 at N.V -> 0, roughness 0.74 - a split sum that
            // reflected 44 % more than it received and doubled every grazing reflection (FOLIAGE-LOOK-3: the pale
            // sheen of Bistro's bush leaves).
            const float a = roughness * roughness;
            const float k = a / 2.0f;

            float scale = 0.0f, bias = 0.0f;
            for ( uint32_t i = 0; i < samples; ++i )
            {
                const float u1 = static_cast<float>( i ) / static_cast<float>( samples );
                const float u2 = RadicalInverseVdC( i );

                const Vec3  H    = ImportanceSampleGGX( u1, u2, roughness );
                const float VdotH = V.x * H.x + V.y * H.y + V.z * H.z;
                const Vec3  L    = { 2.0f * VdotH * H.x - V.x, 2.0f * VdotH * H.y - V.y,
                                     2.0f * VdotH * H.z - V.z };

                const float NdotL = L.z;
                if ( NdotL <= 0.0f )
                    continue;

                const float NdotH = std::max( H.z, 0.0f );
                const float VoH   = std::max( VdotH, 0.0f );

                const float G     = GeometrySchlickGGX( NdotL, k ) * GeometrySchlickGGX( NdotV, k );
                const float GVis  = ( G * VoH ) / std::max( NdotH * NdotV, 1e-5f );
                const float Fc    = std::pow( 1.0f - VoH, 5.0f );

                scale += ( 1.0f - Fc ) * GVis;
                bias += Fc * GVis;
            }

            outScale = scale / static_cast<float>( samples );
            outBias  = bias / static_cast<float>( samples );
        }
    } // namespace

    std::vector<float> GenerateBRDFLutRGBA32F( uint32_t size, uint32_t samples )
    {
        std::vector<float> pixels( static_cast<size_t>( size ) * size * 4, 0.0f );

        // Row-parallel on the engine JobSystem: each row has a fixed roughness; rows are independent.
        Common::JobSystem::Get().ParallelFor(
             size,
             [&]( size_t y )
             {
                 const float roughness = ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( size );
                 for ( uint32_t x = 0; x < size; ++x )
                 {
                     const float NdotV = ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( size );

                     float scale = 0.0f, bias = 0.0f;
                     IntegrateBRDF( NdotV, roughness, samples, scale, bias );

                     float* px = &pixels[( y * size + x ) * 4];
                     px[0]     = scale;
                     px[1]     = bias;
                     px[2]     = 0.0f;
                     px[3]     = 1.0f;
                 }
             } );

        return pixels;
    }
} // namespace Desert::Graphic
