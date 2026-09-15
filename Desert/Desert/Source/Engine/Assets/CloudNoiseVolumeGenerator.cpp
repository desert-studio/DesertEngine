#include <Common/Core/Math/Rounding.hpp>
#include "CloudNoiseVolumeGenerator.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

namespace Desert::Assets
{
    namespace
    {
        // Editor/Resources/Shaders/Common/CloudNoise.glslh, COMPILED AS C++ — the same text, the same file,
        // that Desert/Tests/Engine/CloudNoise drives. This is the SECOND engine translation unit to use the
        // arrangement Graphic::SkyGroundTransmittance established, and for a stronger reason than the
        // first: since the volume became an asset there is no GPU evaluation of these functions at all, so
        // this file IS the noise. A hand-written copy would have nothing left to agree with.
        //
        // The dialect rules are the .glslh's own: glm supplies vec3/vec4 and the maths built-ins with GLSL
        // semantics, `uint` is GLSL's 32-bit word, and the include sits in an ANONYMOUS namespace because
        // GLSL has no `inline` — this translation unit gets its own copy and exports none of it.
        //
        // The shader root is on this project's include path for exactly this kind of include
        // (Desert/Desert/premake5.lua says so).
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;

        using uint = std::uint32_t;

        using glm::clamp;
        using glm::floor;
        using glm::max;
        using glm::min;
        using glm::mix;
        using glm::mod;
        using glm::pow;

#include <Common/CloudNoise.glslh>

        // Eight bits per channel is 1/255 — finer than any erosion threshold can act on, and a quarter of
        // the bandwidth of a half-float volume carrying precision nothing downstream can use. The rounding
        // is to NEAREST rather than truncating: truncation biases every channel down by half a step, which
        // over four channels and eight million voxels is a visible darkening of the whole field.
        unsigned char QuantiseUnit( float value )
        {
            return Common::Math::QuantiseUnitToByte( value );
        }

        // VOXEL CENTRES, NOT CORNERS. Sampling the corner makes the last voxel of the volume carry the same
        // lattice value as the first, which is a half-voxel phase error that shows as a faint plane at the
        // tile boundary — exactly the seam the periodicity was for.
        vec3 VoxelCentre( uint32_t x, uint32_t y, uint32_t z, uint32_t resolution )
        {
            const float inv = 1.0f / static_cast<float>( resolution );
            return vec3( ( static_cast<float>( x ) + 0.5f ) * inv, ( static_cast<float>( y ) + 0.5f ) * inv,
                         ( static_cast<float>( z ) + 0.5f ) * inv );
        }

        vec4 ChannelsAt( const CloudNoiseVolumeParams& params, const vec3& uvw )
        {
            return CloudNoiseVolumeChannels( uvw, params.Seed, params.CurlStrength, params.WispyPeriodLowFrequency,
                                             params.WispyPeriodHighFrequency, params.BillowPeriodLowFrequency,
                                             params.BillowPeriodHighFrequency );
        }
    } // namespace

    void SampleCloudNoiseVolumeChannels( const CloudNoiseVolumeParams& params, float u, float v, float w,
                                         float outChannels[4] )
    {
        const vec4 sampled = ChannelsAt( params, vec3( u, v, w ) );
        outChannels[0]     = sampled.x;
        outChannels[1]     = sampled.y;
        outChannels[2]     = sampled.z;
        outChannels[3]     = sampled.w;
    }

    Common::ResultStr<CloudNoiseVolumeData> GenerateCloudNoiseVolume( const CloudNoiseVolumeParams& params,
                                                                      std::atomic<float>*           progress )
    {
        if ( auto valid = ValidateCloudNoiseVolumeParams( params ); !valid )
            return Common::MakeFormattedError<CloudNoiseVolumeData>( "{}", valid.GetError() );

        CloudNoiseVolumeData data;
        data.Params           = params;
        data.GeneratorVersion = kCloudNoiseGeneratorVersion;
        data.Voxels.resize( static_cast<size_t>( data.VoxelCount() ) * 4u );

        const uint32_t resolution = params.Resolution;

        // The volume is split by Z SLABS and never by voxel, so each thread writes a contiguous run of the
        // output and no two threads touch the same cache line. The result does not depend on the split:
        // every voxel is an independent function of its own coordinate, which is the property that lets the
        // container store a recipe and promise the same bytes back.
        const unsigned hardware = std::thread::hardware_concurrency();
        const uint32_t workers  = std::max( 1u, std::min( resolution, hardware == 0u ? 1u : hardware ) );

        std::atomic<uint32_t> slicesDone{ 0u };

        const auto fillSlabs = [&]( uint32_t worker )
        {
            for ( uint32_t z = worker; z < resolution; z += workers )
            {
                for ( uint32_t y = 0; y < resolution; ++y )
                {
                    for ( uint32_t x = 0; x < resolution; ++x )
                    {
                        const vec4 channels = ChannelsAt( params, VoxelCentre( x, y, z, resolution ) );

                        // x fastest, then y, then z — the tight packing vkCmdCopyBufferToImage expects for a
                        // whole-volume copy, and the layout CloudNoiseVolumeData documents.
                        const size_t index =
                             ( ( static_cast<size_t>( z ) * resolution + y ) * resolution + x ) * 4u;

                        data.Voxels[index + 0] = QuantiseUnit( channels.x );
                        data.Voxels[index + 1] = QuantiseUnit( channels.y );
                        data.Voxels[index + 2] = QuantiseUnit( channels.z );
                        data.Voxels[index + 3] = QuantiseUnit( channels.w );
                    }
                }

                const uint32_t done = slicesDone.fetch_add( 1u ) + 1u;
                if ( progress != nullptr )
                    progress->store( static_cast<float>( done ) / static_cast<float>( resolution ) );
            }
        };

        std::vector<std::thread> threads;
        threads.reserve( workers - 1u );
        for ( uint32_t worker = 1; worker < workers; ++worker )
            threads.emplace_back( fillSlabs, worker );

        fillSlabs( 0u );

        for ( std::thread& thread : threads )
            thread.join();

        if ( progress != nullptr )
            progress->store( 1.0f );

        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets
