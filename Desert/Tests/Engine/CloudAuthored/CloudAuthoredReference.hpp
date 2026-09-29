#pragma once

// Compiles Engine/Content/Shaders/Common/CloudAuthored.glslh and Common/CloudField.glslh AS C++,
// together with the two headers they require, and feeds the seam a REAL sculpted body: the very voxels
// Assets::GenerateCloudModellingVolume writes into a `.dcmv`, read through a trilinear filter that
// reproduces the sampler the device creates.
//
// WHY THAT MATTERS MORE HERE THAN ANYWHERE ELSE IN THIS PROGRAMME. Slot A is the first place where the
// cloud field reads DATA rather than computing a function, so "the volume and its reading agree" is a
// relation with two sides that can drift — the generator's voxel layout and the shader's addressing —
// and neither side can see the other. A test that re-implemented either would be a test of itself. So
// the generator is the engine's own, the addressing is the shader's own text, and what is asserted is
// that they meet.
//
// THE THREE CALLBACKS, and they are the same mechanism CloudFieldReference.hpp uses for the noise and
// the profile table:
//
//     CLOUD_SAMPLE_AUTHORED( uvw )   -> a trilinear, REPEAT-wrapped read of the baked ATLAS
//     CLOUD_AUTHORED_COUNT           -> the instance list this test set up
//     CLOUD_AUTHORED_SLAB_COUNT      -> how many bodies that atlas holds
//     CLOUD_AUTHORED_INSTANCE( i )   -> one of them
//
// The instance list is a mutable global rather than a parameter because the macro's expansion has to be
// an expression and the seam takes no argument for it — which is exactly the shape the storage block
// has on the GPU, so the test drives the same code path with the same indirection.

#include <Engine/Assets/CloudModellingCatalogue.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudAuthoredRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;

        using uint = std::uint32_t;

        using glm::abs;
        using glm::clamp;
        using glm::dot;
        using glm::floor;
        using glm::length;
        using glm::max;
        using glm::min;
        using glm::mix;
        using glm::mod;
        using glm::pow;
        using glm::smoothstep;
        using glm::sqrt;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudNoise.glslh>
#include <Common/CloudGeometry.glslh>

             // ------------------------------------------------------------------------------------------
             // Producer P's two callbacks, identical in construction to CloudFieldReference.hpp's
             // ------------------------------------------------------------------------------------------
             //
             // They are here rather than shared with that header because the two suites are compiled as
             // separate binaries and the dialect namespace is anonymous by construction; what is shared is the
             // SOURCE they both drive, which is the shader text.

             constexpr uint kVolumeSeed     = 1337u;
        constexpr float kCurlStrength   = 0.33f;
        constexpr float kWispyPeriodLF  = 2.0f;
        constexpr float kWispyPeriodHF  = 4.0f;
        constexpr float kBillowPeriodLF = 3.0f;
        constexpr float kBillowPeriodHF = 6.0f;

        using NoiseKey = std::array<std::uint32_t, 3>;

        std::map<NoiseKey, vec4>& NoiseCache()
        {
            static std::map<NoiseKey, vec4> cache;
            return cache;
        }

        /// Every noise slot the seam has asked this suite for, in a set. THE SLOT IS RECORDED RATHER THAN
        /// IGNORED, and the difference is a test: a hero cloud is not a species and names no volume of its
        /// own, so the seam gives every sculpted body species 0's slot — and
        /// TheSculptedBodyIsErodedByTheFirstSpeciesVolume asserts that this set holds nothing else. A
        /// parameter accepted and thrown away would have let the seam start reading whichever volume it
        /// liked with nothing to notice.
        std::set<int>& NoiseSlotsAsked()
        {
            static std::set<int> asked;
            return asked;
        }

        vec4 CloudSampleBakedVolumeCached( int slot, vec3 texturePosition )
        {
            NoiseSlotsAsked().insert( slot );

            NoiseKey key{};
            std::memcpy( key.data(), &texturePosition.x, sizeof( float ) );
            std::memcpy( key.data() + 1, &texturePosition.y, sizeof( float ) );
            std::memcpy( key.data() + 2, &texturePosition.z, sizeof( float ) );

            auto&      cache = NoiseCache();
            const auto it    = cache.find( key );
            if ( it != cache.end() )
                return it->second;

            const vec4 value =
                 CloudNoiseVolumeChannels( texturePosition, kVolumeSeed, kCurlStrength, kWispyPeriodLF,
                                           kWispyPeriodHF, kBillowPeriodLF, kBillowPeriodHF );
            cache.emplace( key, value );
            return value;
        }

#define CLOUD_SAMPLE_NOISE( s, p ) CloudSampleBakedVolumeCached( ( s ), ( p ) )

        // ------------------------------------------------------------------------------------------
        // The procedural producer's modelling volume, which this suite needs bound and does not measure
        // ------------------------------------------------------------------------------------------
        //
        // WHAT THIS SUITE IS ABOUT is slot A — the sculpted body, its addressing, its cutout and the union
        // at the seam. The procedural producer is here only because the seam calls BOTH, so its callback
        // has to exist for this file to compile at all.
        //
        // IT IS A REAL BAKE AND NOT A STUB, and the difference matters for exactly one assertion in the
        // suite: that the union takes the deeper of the two producers' answers. Against a stubbed zero the
        // sculpted side would win everywhere and the union would be untested. The volume is baked once per
        // test binary from the built-in type, at a coverage low enough that most of the sky is clear —
        // which is the sky a hero cloud is placed into.
        struct ProceduralState
        {
            std::vector<unsigned char>                 Voxels;
            Desert::Assets::CloudProceduralFieldParams Params;
            glm::vec2                                  OriginKm{ 0.0f };
        };

        /// The sky the seam's OTHER producer is putting up, at a given coverage.
        ///
        /// KEYED ON THE COVERAGE AND CACHED, because a bake is a second and a half in a debug build and
        /// this suite asks for two skies: a sparse one for the ordinary tests and an overcast one for the
        /// cutout, which can only be measured where there IS procedural field to remove.
        const ProceduralState& Procedural( float coverage = 0.25f )
        {
            static std::map<int, ProceduralState> cache;

            const int key = static_cast<int>( coverage * 1000.0f + 0.5f );

            const auto it = cache.find( key );
            if ( it != cache.end() )
                return it->second;

            ProceduralState built;

            const Desert::Graphic::CloudTypeShape shape = Desert::Assets::CloudTypeDefaultShape();

            built.Params.RegionSizeKm = 48.0f;

            const Desert::Graphic::CloudEnvelopeKm envelope =
                 Desert::Graphic::CloudTypeSetEnvelopeKm( &shape, 1u );

            built.Params.LayerBottomKm     = std::max( envelope.BottomKm, 0.0f );
            built.Params.LayerThicknessKm  = std::max( envelope.TopKm - built.Params.LayerBottomKm, 0.001f );
            built.Params.BlendRadiusKm     = 0.06f;
            built.Params.ProfileDepthKm    = 0.36f;
            built.Params.Coverage          = coverage;
            built.Params.CoverageContrast  = 1.0f;
            built.Params.Seed              = 1u;
            built.Params.WindAxis          = glm::vec2( 1.0f, 0.0f );
            built.Params.ResolvableChordKm = Desert::Graphic::CloudFinestResolvableChordKm( 256.0f );

            Desert::Assets::CloudProceduralSpecies species;
            species.Shape      = shape;
            species.CellKm     = 3.0f;
            species.Anisotropy = 1.0f;
            built.Params.Species.push_back( species );

            built.OriginKm = Desert::Assets::CloudProceduralRegionOriginKm( built.Params, 0.0f, 0.0f );

            const auto baked = Desert::Assets::BakeCloudProceduralVolume( built.Params, built.OriginKm );
            if ( baked )
                built.Voxels = baked.GetValue();

            return cache.emplace( key, std::move( built ) ).first->second;
        }

        /// WHICH SKY IS BOUND RIGHT NOW. The callback below cannot take an argument — it is the shader's
        /// own fetch — so the choice is a piece of state, set by ParamsAtCoverage and read here.
        float& BoundCoverage()
        {
            static float coverage = 0.25f;
            return coverage;
        }

        /// A trilinear, REPEAT-wrapped fetch, as VulkanImage3D creates every sampled volume.
        vec4 CloudSampleProceduralTexture( vec3 uvw )
        {
            const std::vector<unsigned char>& voxels = Procedural( BoundCoverage() ).Voxels;
            if ( voxels.empty() )
                return vec4( 0.0f );

            constexpr int width  = static_cast<int>( Desert::Assets::kCloudProceduralVolumeSide );
            constexpr int height = static_cast<int>( Desert::Assets::kCloudProceduralVolumeHeight );
            constexpr int depth  = static_cast<int>( Desert::Assets::kCloudProceduralVolumeSide );

            const float x = uvw.x * static_cast<float>( width ) - 0.5f;
            const float y = uvw.y * static_cast<float>( height ) - 0.5f;
            const float z = uvw.z * static_cast<float>( depth ) - 0.5f;

            const float fx = x - std::floor( x );
            const float fy = y - std::floor( y );
            const float fz = z - std::floor( z );

            const auto wrap = []( float coordinate, int extent )
            {
                const int index = static_cast<int>( std::floor( coordinate ) ) % extent;
                return index < 0 ? index + extent : index;
            };

            const int x0 = wrap( x, width );
            const int y0 = wrap( y, height );
            const int z0 = wrap( z, depth );
            const int x1 = ( x0 + 1 ) % width;
            const int y1 = ( y0 + 1 ) % height;
            const int z1 = ( z0 + 1 ) % depth;

            const auto texel = [&]( int ix, int iy, int iz )
            {
                const size_t base = ( ( static_cast<size_t>( iz ) * height + iy ) * width + ix ) *
                                    Desert::Assets::kCloudProceduralBytesPerVoxel;
                return vec4( voxels[base] / 255.0f, voxels[base + 1] / 255.0f, voxels[base + 2] / 255.0f,
                             voxels[base + 3] / 255.0f );
            };

            const auto plane = [&]( int iz )
            {
                const vec4 top    = texel( x0, y0, iz ) * ( 1.0f - fx ) + texel( x1, y0, iz ) * fx;
                const vec4 bottom = texel( x0, y1, iz ) * ( 1.0f - fx ) + texel( x1, y1, iz ) * fx;
                return top * ( 1.0f - fy ) + bottom * fy;
            };

            return plane( z0 ) * ( 1.0f - fz ) + plane( z1 ) * fz;
        }

#define CLOUD_SAMPLE_MODELLING( p ) CloudSampleProceduralTexture( p )

        // ------------------------------------------------------------------------------------------
        // Producer A: the baked body, and the device's own filter over it
        // ------------------------------------------------------------------------------------------

        /// The shipped example, baked ONCE per test binary. About a second and a half in a debug build,
        /// which is why it is a lazy static rather than a fixture member: every test in this suite reads
        /// the same body, and baking it per test would turn a two-second suite into a minute.
        const Desert::Assets::CloudModellingVolumeData& Body()
        {
            static const Desert::Assets::CloudModellingVolumeData body = []
            {
                Desert::Assets::CloudModellingVolumeData data;
                data.Recipe = Desert::Assets::CloudModellingDefaultRecipe();

                auto voxels = Desert::Assets::GenerateCloudModellingVolume( data.Recipe );
                if ( voxels )
                    data.Voxels = voxels.ExtractValue();
                return data;
            }();
            return body;
        }

        /// A second and a third body, so this suite can drive the atlas with bodies that are DIFFERENT
        /// and not merely repeated. Two of the catalogue's genera, chosen because the arch and the
        /// cumulonimbus disagree everywhere: what a test wants from a second slab is that reading the
        /// wrong one is visible, and two similar clouds would hide exactly that.
        const std::vector<unsigned char>& CatalogueBody( Desert::Assets::CloudModellingSpecies species )
        {
            static std::map<uint32_t, std::vector<unsigned char>> baked;

            const uint32_t key = static_cast<uint32_t>( species );
            const auto     it  = baked.find( key );
            if ( it != baked.end() )
                return it->second;

            auto voxels = Desert::Assets::GenerateCloudModellingVolume(
                 Desert::Assets::CloudModellingCatalogueRecipe( species ) );
            return baked.emplace( key, voxels ? voxels.ExtractValue() : std::vector<unsigned char>{} )
                 .first->second;
        }

        // THE ATLAS THE SEAM READS, in the shape the device has it: one byte array holding N bodies end to
        // end along the depth axis, assembled by the engine's own Assets::AssembleCloudModellingAtlas
        // rather than by a copy written here — a test that re-implemented the packing would be a test of
        // itself, which is the same rule the generator is driven by above.
        std::vector<unsigned char> g_Atlas;
        int                        g_AuthoredSlabCount = 0;

        /// What g_Atlas was last built from, so that resetting to the ordinary one-body case between two
        /// thousand probes does not re-assemble four megabytes two thousand times.
        std::vector<const std::vector<unsigned char>*> g_AtlasSources;

        void SetAtlas( const std::vector<const std::vector<unsigned char>*>& bodies )
        {
            if ( bodies == g_AtlasSources )
                return;

            const auto assembled = Desert::Assets::AssembleCloudModellingAtlas( bodies );
            g_Atlas              = assembled ? assembled.GetValue() : std::vector<unsigned char>{};
            g_AuthoredSlabCount  = assembled ? static_cast<int>( bodies.size() ) : 0;
            g_AtlasSources       = assembled ? bodies : std::vector<const std::vector<unsigned char>*>{};
        }

        /// The ordinary case: one body, one slab — which is what A0 and A1 shipped and what the six-point
        /// protocol renders.
        void SetSingleBodyAtlas()
        {
            SetAtlas( { &Body().Voxels } );
        }

        /**
         * A trilinear, REPEAT-wrapped fetch of the baked voxels — the filter and the address mode
         * VulkanImage3D creates for every volume this engine uploads.
         *
         * WRITTEN OUT RATHER THAN APPROXIMATED, because the half-texel offset is the whole point: the
         * shader clamps its coordinate to the texel centres before it fetches (CloudAuthoredClampUvw), and
         * a nearest-neighbour reference would agree with a broken clamp exactly as often as with a correct
         * one.
         */
        vec4 CloudSampleAuthoredVolume( vec3 uvw )
        {
            const std::vector<unsigned char>& body = g_Atlas;

            constexpr int width  = static_cast<int>( Desert::Assets::kCloudModellingVolumeWidth );
            constexpr int height = static_cast<int>( Desert::Assets::kCloudModellingVolumeHeight );
            const int     depth  = static_cast<int>( Desert::Assets::kCloudModellingVolumeDepth ) *
                              std::max( g_AuthoredSlabCount, 1 );

            const float x = uvw.x * static_cast<float>( width ) - 0.5f;
            const float y = uvw.y * static_cast<float>( height ) - 0.5f;
            const float z = uvw.z * static_cast<float>( depth ) - 0.5f;

            const float fx = x - std::floor( x );
            const float fy = y - std::floor( y );
            const float fz = z - std::floor( z );

            const auto wrap = []( float coordinate, int extent )
            {
                const int index = static_cast<int>( std::floor( coordinate ) ) % extent;
                return index < 0 ? index + extent : index;
            };

            const int x0 = wrap( x, width );
            const int y0 = wrap( y, height );
            const int z0 = wrap( z, depth );
            const int x1 = ( x0 + 1 ) % width;
            const int y1 = ( y0 + 1 ) % height;
            const int z1 = ( z0 + 1 ) % depth;

            const auto texel = [&]( int ix, int iy, int iz )
            {
                const size_t base = ( ( static_cast<size_t>( iz ) * height + iy ) * width + ix ) *
                                    Desert::Assets::kCloudModellingBytesPerVoxel;
                return vec4( static_cast<float>( body[base + 0] ) / 255.0f,
                             static_cast<float>( body[base + 1] ) / 255.0f,
                             static_cast<float>( body[base + 2] ) / 255.0f,
                             static_cast<float>( body[base + 3] ) / 255.0f );
            };

            const auto lerp3 = []( const vec4& a, const vec4& b, float t ) { return a * ( 1.0f - t ) + b * t; };

            const vec4 c00 = lerp3( texel( x0, y0, z0 ), texel( x1, y0, z0 ), fx );
            const vec4 c10 = lerp3( texel( x0, y1, z0 ), texel( x1, y1, z0 ), fx );
            const vec4 c01 = lerp3( texel( x0, y0, z1 ), texel( x1, y0, z1 ), fx );
            const vec4 c11 = lerp3( texel( x0, y1, z1 ), texel( x1, y1, z1 ), fx );

            return lerp3( lerp3( c00, c10, fy ), lerp3( c01, c11, fy ), fz );
        }

#define CLOUD_SAMPLE_AUTHORED( uvw ) CloudSampleAuthoredVolume( uvw )

        // The instance list the seam reads, in the shape the storage block has on the GPU: an array and a
        // count, reached through a macro rather than passed as an argument.
        int                                       g_AuthoredCount = 0;
        Desert::Graphic::CloudAuthoredInstanceGpu g_AuthoredInstances[Desert::Graphic::kCloudAuthoredSlots]{};

#define CLOUD_AUTHORED_COUNT g_AuthoredCount
#define CLOUD_AUTHORED_SLAB_COUNT g_AuthoredSlabCount
#define CLOUD_AUTHORED_INSTANCE( i ) CloudAuthoredInstanceAt( i )

#include <Common/CloudAuthored.glslh>

        /**
         * The bridge between the C++ payload and the dialect's own struct — and the ASSERTION that the two
         * are one layout.
         *
         * It is a `memcpy` and not a field-by-field copy on purpose. Graphic::CloudAuthoredInstanceGpu is
         * what the renderer writes into the storage buffer and `CloudAuthoredInstance` is what the shader
         * reads out of it; on the device that IS a reinterpretation of the same bytes, so a test that
         * copied member by member would be testing a translation nothing performs. Copying the bytes means
         * a member inserted into one and not the other is caught here rather than in a frame.
         */
        CloudAuthoredInstance CloudAuthoredInstanceAt( int index )
        {
            static_assert( sizeof( CloudAuthoredInstance ) == sizeof( Desert::Graphic::CloudAuthoredInstanceGpu ),
                           "The dialect's instance and the payload's instance are one layout." );

            CloudAuthoredInstance instance;
            std::memcpy( &instance, &g_AuthoredInstances[index], sizeof( instance ) );
            return instance;
        }

#include <Common/CloudField.glslh>
        DESERT_GLSL_AS_CPP_END

        // ------------------------------------------------------------------------------------------
        // Setting the scene
        // ------------------------------------------------------------------------------------------

        /// Empties the instance list and NOTHING else. The atlas stays bound, which is what a sweep over
        /// thousands of positions needs: re-assembling a three-body atlas per probe is twelve megabytes of
        /// memcpy per probe, and a suite that took eight minutes to say something true is a suite nobody
        /// runs.
        void ClearInstanceList()
        {
            g_AuthoredCount = 0;
            for ( auto& instance : g_AuthoredInstances )
                instance = Desert::Graphic::CloudAuthoredInstanceGpu{};
        }

        /// Empties the instance list AND puts the atlas back to the one-body case, because a test that
        /// left three slabs bound would hand the next one a body it never asked for — and the whole point
        /// of the slab index is that reading the wrong one is silent.
        void ClearInstances()
        {
            ClearInstanceList();
            SetSingleBodyAtlas();
        }

        void AddInstance( const Desert::Graphic::CloudAuthoredInstanceGpu& instance )
        {
            g_AuthoredInstances[g_AuthoredCount] = instance;
            ++g_AuthoredCount;
        }

        /// The layer the procedural producer is measured against here: the built-in cumulus congestus,
        /// which is what an empty slot resolves to and therefore what Clouds_Demo renders.
        /// The layer at a chosen coverage. It BAKES, because coverage decides what is in the volume now
        /// rather than what threshold the march applies to a noise — so a test that wants an overcast sky
        /// asks for one here instead of assigning a field.
        CloudFieldParams ParamsAtCoverage( float coverage );

        CloudFieldParams DefaultParams()
        {
            const Desert::Graphic::CloudTypeShape& shape = Desert::Assets::CloudTypeDefaultShape();

            CloudFieldParams params;
            params.DetailTileKm   = 1.2f;
            params.DetailStrength = 0.35f;
            params.DensityScale   = 1.0f;
            params.SpeciesCount   = 1;
            params.WindOffsetKm   = vec3( 0.0f );

            // The C++ mirror marches as the EYE does. A CloudFieldParams built member by member
            // here bypasses CloudUnpackFieldParams, which is where the shader sets this, so leaving it
            // out would hand the reference an indeterminate float -- the fixture-fills-three-of-five
            // hazard this suite has already been bitten by once.
            params.ShadowRay = CLOUD_RAY_VIEW;

            for ( int slot = 0; slot < CLOUD_SPECIES_SLOTS; ++slot )
            {
                params.SpeciesEdge[slot] = vec4( 0.0f );
                // One species and therefore one volume, which is what a layer of one type resolves to and
                // what every scene in the repository is in. The point of the assertion this suite adds is
                // that the SCULPTED side asks for this slot too.
                params.SpeciesNoise[slot] = 0;
            }

            params.SpeciesEdge[0] =
                 vec4( shape.DetailCharacter, shape.DetailFactor, shape.DensityFactor, shape.ExtinctionFactor );

            // WHERE THE PROCEDURAL VOLUME IS, which is what the placement basis used to be. The tile, the
            // coverage and its contrast are not fields of this struct any more: they decide what is IN the
            // volume and are consumed by the bake, so they are set on Procedural() above and reach the
            // seam through the bytes.
            params.RegionOriginKm  = Procedural( BoundCoverage() ).OriginKm;
            params.InvRegionSizeKm = 1.0f / Procedural( BoundCoverage() ).Params.RegionSizeKm;

            return params;
        }
        CloudFieldParams ParamsAtCoverage( float coverage )
        {
            BoundCoverage() = coverage;
            return DefaultParams();
        }
    } // namespace
} // namespace Desert::Tests::CloudAuthoredRef
