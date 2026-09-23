#pragma once

// Compiles Editor/Resources/Shaders/Common/CloudField.glslh AS C++, together with the two headers it
// requires: Common/CloudNoise.glslh (the noise it is fed) and Common/CloudGeometry.glslh (the types and
// the units it shares).
//
// THE NOISE CALLBACK IS THE POINT OF THE SEAM. CloudField.glslh declares no sampler; it asks for the four
// octaves through CLOUD_SAMPLE_NOISE, which the march defines as a fetch of the baked volume and this
// header defines as a call into the very function that GENERATES it — CloudNoiseVolumeChannels, the same
// text Engine/Assets/CloudNoiseVolumeGenerator.cpp compiles to write the file. A test written against a
// different field would be a test of a different sky.
//
// The two differences from the GPU path are deliberate and stated rather than discovered:
//   * the volume is RGBA8, so the GPU sees the field quantized to 1/255 and trilinearly interpolated
//     between 128 voxels per axis, while this evaluates it analytically. Nothing asserted here is finer
//     than that quantization.
//   * the parameters are the volume asset's defaults, because that is what the shipped
//     CloudNoise_Default.dcnv carries; a different seed moves individual clouds and not one of the
//     statistics measured here.
//
// THE RESULTS ARE MEMOIZED. The coverage measurement evaluates the same grid of positions once per
// Coverage setting, and the field depends only on the position — so the cache turns a six-fold cost into
// a one-fold one and changes no answer. It lives here rather than in the test because the macro has to be
// defined before CloudField.glslh is included.

// THE MODELLING VOLUME ARRIVES THE SAME WAY THE NOISE DOES, and that is what makes the second half of
// this header a test of the GPU's arithmetic rather than of a re-implementation. CLOUD_SAMPLE_MODELLING is
// defined below as a TRILINEAR, REPEAT-WRAPPED read of the very bytes Assets::BakeCloudProceduralVolume
// hands the device — the same filter and the same wrap mode every sampler in this engine is created with —
// so `what the generator writes` and `what the shader reads` can be compared directly, which is the one
// relation a volume nobody can inspect on the GPU would otherwise never be checked on.
//
// IT REPLACED A PROFILE TABLE, and the replacement is the whole of phase Э5 at this seam. The table was
// `f(height in the envelope, how deep inside the patch)` multiplied by a threshold on the Alligator noise
// — whose field is `best - second` and is therefore ZERO wherever two feature points contribute equally,
// so no setting of any slider could fuse two lobes. Everything in this header below the callbacks is
// unchanged; the tests that measured the table's own shape moved to
// Desert/Tests/Engine/CloudProceduralField, which owns the generator that now decides it.

#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudFieldRef
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

             // ONE call into the ONE function that writes the volume — Common/CloudNoise.glslh's
             // CloudNoiseVolumeChannels — rather than four calls this file has to keep in step with the
             // generator's. The four-line copy that used to be here mirrored the compute bake and was a second
             // statement of the channel layout; it is exactly the shape of duplication that agrees with itself
             // until the first tuning pass.
             //
             // The parameters are the defaults of Engine/Assets/CloudNoiseVolume.hpp's CloudNoiseVolumeParams,
             // which is what the shipped CloudNoise_Default.dcnv was baked with. A different seed moves
             // individual clouds and not one of the statistics measured here.
             constexpr uint kVolumeSeed     = 1337u;
        constexpr float kCurlStrength   = 0.33f;
        constexpr float kWispyPeriodLF  = 2.0f;
        constexpr float kWispyPeriodHF  = 4.0f;
        constexpr float kBillowPeriodLF = 3.0f;
        constexpr float kBillowPeriodHF = 6.0f;

        // The number of noise volumes a layer can bind, taken from the C++ constant because
        // CLOUD_SPECIES_SLOTS is declared by Common/CloudField.glslh, which is included BELOW this block —
        // the callbacks have to exist before the header that calls them. Desert/Tests/Engine/CloudField
        // asserts the two agree once both are in scope.
        constexpr int kNoiseSlots = static_cast<int>( Graphic::kCloudSpeciesSlots );

        /// The four lattice periods of one BOUND VOLUME, in the order CloudNoiseVolumeChannels takes them.
        struct BoundVolumePeriods
        {
            float WispyLF, WispyHF, BillowLF, BillowHF;
        };

        /**
         * The four volumes a layer can bind, as the reference sees them.
         *
         * SLOT 0 IS THE SHIPPED DEFAULT and every other slot starts as a COPY of it, because that is the
         * state a layer whose types name no volume of their own is in — and it is the state every
         * assertion written before a type could name one was written against. A test that wants a second
         * volume calls CloudNoiseVolumeSelect, which is the same thing the renderer does when an artist
         * drops a `.dcnv` into a type's slot.
         *
         * The second row this suite actually uses is the shipped CloudNoise_FineWisp.dcnv's — 4/8/6/12,
         * exactly twice the default on every channel. It is the one volume in the repository that is not
         * the default, so a mirror that used invented numbers would be testing a sky nobody ships.
         */
        struct BoundVolumes
        {
            BoundVolumePeriods Slot[kNoiseSlots];
        };

        BoundVolumes DefaultBoundVolumes()
        {
            BoundVolumes fresh{};
            for ( int slot = 0; slot < kNoiseSlots; ++slot )
                fresh.Slot[slot] =
                     BoundVolumePeriods{ kWispyPeriodLF, kWispyPeriodHF, kBillowPeriodLF, kBillowPeriodHF };
            return fresh;
        }

        BoundVolumes& NoiseVolumeState()
        {
            static BoundVolumes state = DefaultBoundVolumes();
            return state;
        }

        /// The periods of the shipped `CloudNoise_FineWisp.dcnv`: twice the default's frequency on every
        /// channel, which is the whole of what makes a cirrus' edge a different SIZE from a cumulus'.
        constexpr BoundVolumePeriods kFineWispPeriods{ 4.0f, 8.0f, 6.0f, 12.0f };

        vec4 CloudEvaluateBakedVolume( int slot, vec3 texturePosition )
        {
            const BoundVolumePeriods& p = NoiseVolumeState().Slot[std::clamp( slot, 0, kNoiseSlots - 1 )];
            return CloudNoiseVolumeChannels( texturePosition, kVolumeSeed, kCurlStrength, p.WispyLF, p.WispyHF,
                                             p.BillowLF, p.BillowHF );
        }

        // Exact-key memoization: the key is the SLOT and the bit pattern of the three coordinates, so two
        // callers that ask the same volume for the same position get the same answer and nothing is ever
        // interpolated. THE SLOT IS PART OF THE KEY and not an afterthought — without it the cache would
        // hand slot 1 the answer slot 0 gave, which is precisely the defect this whole task is about,
        // reproduced inside its own test.
        using NoiseKey = std::array<std::uint32_t, 4>;

        std::map<NoiseKey, vec4>& NoiseCache()
        {
            static std::map<NoiseKey, vec4> cache;
            return cache;
        }

        vec4 CloudSampleBakedVolumeCached( int slot, vec3 texturePosition )
        {
            NoiseKey key{};
            key[0] = static_cast<std::uint32_t>( std::clamp( slot, 0, kNoiseSlots - 1 ) );
            std::memcpy( key.data() + 1, &texturePosition.x, sizeof( float ) );
            std::memcpy( key.data() + 2, &texturePosition.y, sizeof( float ) );
            std::memcpy( key.data() + 3, &texturePosition.z, sizeof( float ) );

            auto&      cache = NoiseCache();
            const auto it    = cache.find( key );
            if ( it != cache.end() )
                return it->second;

            const vec4 value = CloudEvaluateBakedVolume( slot, texturePosition );
            cache.emplace( key, value );
            return value;
        }

        /// Bind @p periods into one of the four slots, and drop the cached answers that came from whatever
        /// was there before. Same effect as the artist naming a different `.dcnv` on a cloud type.
        void CloudNoiseVolumeSelect( int slot, const BoundVolumePeriods& periods )
        {
            NoiseVolumeState().Slot[std::clamp( slot, 0, kNoiseSlots - 1 )] = periods;
            NoiseCache().clear();
        }

        /// Put every slot back on the shipped default, which is the state every assertion that does not
        /// mention a volume assumes.
        void CloudNoiseVolumeResetAll()
        {
            NoiseVolumeState() = DefaultBoundVolumes();
            NoiseCache().clear();
        }

#define CLOUD_SAMPLE_NOISE( s, p ) CloudSampleBakedVolumeCached( ( s ), ( p ) )

        // ------------------------------------------------------------------------------------------
        // The procedural modelling volume, exactly as the device would see it
        // ------------------------------------------------------------------------------------------

        // WHICH SKY THE BOUND VOLUME IS. A test that wants another one calls CloudModellingVolumeSelect
        // and the next read comes from the new bake — the same thing the renderer does when the artist
        // drops a different `.decloudtype` into a slot, and from the same single source
        // (Assets::BakeCloudProceduralVolume).
        /// The bytes of one bake, shared by every binding that asked for the same sky.
        using ModellingVoxels = std::shared_ptr<const std::vector<unsigned char>>;

        struct ModellingVolumeState
        {
            /// NULL UNTIL A TEST BINDS A SPECIES, which is the state the empty vector used to carry, and
            /// the state CloudSampleModellingTexture still answers with a zeroed fetch. Shared rather than
            /// owned so that binding a sky this suite has already baked costs a refcount and not eight
            /// megabytes — see CloudModellingBake.
            ModellingVoxels                            Voxels;
            Desert::Assets::CloudProceduralFieldParams Params;
            glm::vec2                                  OriginKm{ 0.0f };
        };

        ModellingVolumeState& ModellingVolume()
        {
            static ModellingVolumeState state;
            return state;
        }

        // ------------------------------------------------------------------------------------------
        // ONE BAKE PER DISTINCT SKY, AND NOT ONE PER BINDING
        // ------------------------------------------------------------------------------------------
        //
        // THE NUMBER THAT MADE THIS WORTH WRITING. Baking the modelling volume is 91 % of this suite's
        // run time — measured with `sample` against the ASan+UBSan Debug binary CI builds, where the
        // main thread and all nine JobSystem workers sit inside BakeCloudProceduralVolume's parallel
        // range for the whole window, in CloudModellingBlobDistanceKm and the glm vector arithmetic
        // under it. The sampling this suite does on top of the volume is the other 9 %.
        //
        // AND IT WAS BAKED FORTY-EIGHT TIMES FOR NINETEEN DISTINCT SKIES, because every call to
        // CloudBindSpecies re-baked unconditionally. The worst of it is visible in the two coverage
        // sweeps: CloudFieldCoverage walks Coverage 0.0 to 1.0 in eleven steps, and
        // CloudSunTransmittance then walks THE SAME ELEVEN, re-baking two million voxels apiece for
        // volumes that had just been produced and thrown away.
        //
        // THE ENGINE ALREADY KNOWS WHICH TWO REQUESTS ARE THE SAME REQUEST. Assets::
        // CloudProceduralParamsEqual is the editor's own answer to "must this be re-baked", written so
        // that a preview does not stall on a slider that provably changed nothing; this suite is the
        // one caller that was not asking. Using it here rather than comparing the arguments the test
        // passes is deliberate: the parameters ARE the bake's input, so a field added to them cannot
        // go uncompared by a key that was written to mirror them.
        //
        // NOTHING MEASURED CHANGES. The bake is a pure function of (parameters, region origin) — the
        // seed is fixed and the join is order-independent — so a hit hands back the bytes the call
        // would have produced, and every assertion is made against the volume it was made against
        // before. What changes is only how many times those bytes are computed.
        //
        // THE CACHE IS BOUNDED, and the bound is enforced rather than asserted in a comment: past
        // kMaxBakedVolumes the oldest entry is dropped, which costs a later miss and never a wrong
        // answer. Eight megabytes a volume (256 x 32 x 256 x 4 bytes), so the ceiling is 256 MiB.
        constexpr size_t kMaxBakedVolumes = 32;

        struct BakedVolume
        {
            Desert::Assets::CloudProceduralFieldParams Params;
            glm::vec2                                  OriginKm{ 0.0f };
            ModellingVoxels                            Voxels;
        };

        std::vector<BakedVolume>& BakedVolumeCache()
        {
            static std::vector<BakedVolume> cache;
            return cache;
        }

        /// How many bakes were RUN and how many were SERVED, counted rather than claimed.
        ///
        /// IT IS REPORTED IN THE SUITE'S OUTPUT, and that is the point of it. Wall clock on this suite is
        /// bimodal on the machine it was measured on — two clean runs of the same binary came back at
        /// 834.29 s and 834.43 s and a third at 630.90 s, a 32 % spread with no change in between — so a
        /// timing is not, by itself, evidence that work was removed. These two counters are: they are the
        /// same integers on any machine, and `Run + Served` is exactly the number of bakes the code
        /// before this cache performed.
        struct BakeTally
        {
            int Run    = 0;
            int Served = 0;
        };

        BakeTally& BakeCounts()
        {
            static BakeTally tally;
            return tally;
        }

        /// Bake @p params over @p originKm, or hand back the bytes an identical request already made.
        ModellingVoxels CloudModellingBake( const Desert::Assets::CloudProceduralFieldParams& params,
                                            const glm::vec2&                                  originKm )
        {
            std::vector<BakedVolume>& cache = BakedVolumeCache();

            for ( const BakedVolume& entry : cache )
            {
                if ( entry.OriginKm == originKm &&
                     Desert::Assets::CloudProceduralParamsEqual( entry.Params, params ) )
                {
                    ++BakeCounts().Served;
                    return entry.Voxels;
                }
            }

            ++BakeCounts().Run;

            const auto baked = Desert::Assets::BakeCloudProceduralVolume( params, originKm );

            // A FAILED BAKE IS CACHED TOO, and on purpose: it is a pure function of the same inputs, so
            // re-running it would spend the same minutes to arrive at the same empty answer, and the
            // tests read an empty volume as a sky with no cloud in it either way.
            ModellingVoxels voxels = std::make_shared<const std::vector<unsigned char>>(
                 baked ? baked.GetValue() : std::vector<unsigned char>{} );

            if ( cache.size() >= kMaxBakedVolumes )
                cache.erase( cache.begin() );

            cache.push_back( BakedVolume{ params, originKm, voxels } );
            return voxels;
        }

        /// The parameters this suite bakes with: one region, centred on the origin, at the component's
        /// own defaults. The species are handed in, so a test drives exactly the set a layer would carry.
        Desert::Assets::CloudProceduralFieldParams
        CloudModellingParams( const Desert::Graphic::CloudTypeShape* shapes, std::uint32_t count, float coverage,
                              float contrast, vec3 windDirection )
        {
            Desert::Assets::CloudProceduralFieldParams params;

            params.RegionSizeKm = 48.0f;

            const Desert::Graphic::CloudEnvelopeKm envelope =
                 Desert::Graphic::CloudTypeSetEnvelopeKm( shapes, count );

            params.LayerBottomKm    = std::max( envelope.BottomKm, 0.0f );
            params.LayerThicknessKm = std::max( envelope.TopKm - params.LayerBottomKm, 0.001f );

            const float latticeKm = 3.0f;

            params.BlendRadiusKm     = 0.02f * latticeKm;
            params.ProfileDepthKm    = 0.12f * latticeKm;
            params.Coverage          = coverage;
            params.CoverageContrast  = contrast;
            params.Seed              = 1u;
            params.WindAxis          = glm::vec2( windDirection.x, windDirection.z );
            params.ResolvableChordKm = Desert::Graphic::CloudFinestResolvableChordKm( 256.0f );

            for ( std::uint32_t slot = 0; slot < count; ++slot )
            {
                Desert::Assets::CloudProceduralSpecies species;
                species.Shape      = shapes[slot];
                species.CellKm     = latticeKm * std::max( shapes[slot].PlacementScale, 1e-3f );
                species.Anisotropy = std::max( shapes[slot].PlacementAnisotropy, 1e-3f );
                params.Species.push_back( species );
            }

            return params;
        }

        void CloudModellingVolumeSelectSet( const Desert::Graphic::CloudTypeShape* shapes, std::uint32_t count,
                                            float coverage, float contrast, vec3 windDirection )
        {
            ModellingVolumeState& state = ModellingVolume();

            state.Params   = CloudModellingParams( shapes, count, coverage, contrast, windDirection );
            state.OriginKm = Desert::Assets::CloudProceduralRegionOriginKm( state.Params, 0.0f, 0.0f );
            state.Voxels   = CloudModellingBake( state.Params, state.OriginKm );
        }

        /// The same bake over a layer WIDER than the species' own band, which is what makes the vertical
        /// clamp observable: the volume's top rows are then empty air and its bottom rows are cloud, so a
        /// read at a height fraction of 1 that wrapped onto the floor would come back with cloud in it.
        ///
        /// A layer is normally the union of its types' bands exactly (Graphic::CloudTypeSetEnvelopeKm), so
        /// this arrangement does not arise by itself — which is precisely why the property needs a fixture
        /// that produces it rather than a sample of an ordinary sky.
        void CloudModellingVolumeSelectOverLayer( const Desert::Graphic::CloudTypeShape* shapes,
                                                  std::uint32_t count, float coverage, float bottomKm,
                                                  float thicknessKm )
        {
            ModellingVolumeState& state = ModellingVolume();

            state.Params = CloudModellingParams( shapes, count, coverage, 1.0f, vec3( 1.0f, 0.0f, 0.0f ) );

            state.Params.LayerBottomKm    = bottomKm;
            state.Params.LayerThicknessKm = thicknessKm;

            state.OriginKm = Desert::Assets::CloudProceduralRegionOriginKm( state.Params, 0.0f, 0.0f );
            state.Voxels   = CloudModellingBake( state.Params, state.OriginKm );
        }

        // A TRILINEAR, REPEAT-wrapped fetch — the filter and the address mode VulkanImage3D creates for
        // every sampled volume, written out here because the difference between this and a nearest fetch
        // is exactly the half-texel error the relation test exists to catch.
        vec4 CloudSampleModellingTexture( vec3 uvw )
        {
            const ModellingVoxels& bytes = ModellingVolume().Voxels;
            if ( !bytes || bytes->empty() )
                return vec4( 0.0f );

            const std::vector<unsigned char>& voxels = *bytes;

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

#define CLOUD_SAMPLE_MODELLING( p ) CloudSampleModellingTexture( p )

        // ------------------------------------------------------------------------------------------
        // SLOT A, DECLARED EMPTY — this suite drives producer P
        // ------------------------------------------------------------------------------------------
        //
        // The seam calls both producers now, so the authored one's four callbacks have to exist for this
        // file to compile at all. They are bound to an EMPTY list here, deliberately: what this suite
        // measures is the procedural field's own statistics — its quantiles, its coverage, its erosion —
        // and every one of them is a number about a sky with no hero cloud in it.
        //
        // THAT MAKES THIS SUITE THE REGRESSION TEST FOR "P DID NOT CHANGE". Every assertion in it was
        // written before slot A existed and none of them was touched; if the union, the cutout or the
        // early-out had altered the procedural answer by so much as a quantisation step, these numbers
        // would have moved. Producer A has its own suite, Desert/Tests/Engine/CloudAuthored.
#define CLOUD_AUTHORED_COUNT 0
#define CLOUD_AUTHORED_SLAB_COUNT 0
#define CLOUD_AUTHORED_INSTANCE( i ) CloudAuthoredNoInstance()
// The stub returns zero, but it EVALUATES its argument, and that is not cosmetic. Written as a bare
// `vec4( 0, 0, 0, 0 )` it discarded the addressing expression entirely, so `CloudAuthoredAtlasUvw` and
// the `slabCount` handed to it were never compiled into anything here — which is exactly what
// `-Wunused-variable` was reporting about `CloudField.glslh:384`. This suite deliberately runs producer
// A's loop zero times (CLOUD_AUTHORED_COUNT is 0, and Desert/Tests/Engine/CloudAuthored owns that
// producer), so the value is still zero; what changes is that the atlas addressing type-checks here too.
#define CLOUD_SAMPLE_AUTHORED( uvw ) ( (void)( uvw ), vec4( 0.0f, 0.0f, 0.0f, 0.0f ) )

#include <Common/CloudAuthored.glslh>

        // Never called: the loop that would call it runs zero times. It exists because a macro has to
        // expand to something that compiles, and returning a zeroed instance is the only expansion that
        // cannot be mistaken for a real one if the count ever stops being zero by accident.
        CloudAuthoredInstance CloudAuthoredNoInstance()
        {
            CloudAuthoredInstance instance;
            instance.Row0      = vec4( 0.0f );
            instance.Row1      = vec4( 0.0f );
            instance.Row2      = vec4( 0.0f );
            instance.BoundsMin = vec4( 0.0f );
            instance.BoundsMax = vec4( 0.0f );
            return instance;
        }

#include <Common/CloudField.glslh>
        DESERT_GLSL_AS_CPP_END

        // ------------------------------------------------------------------------------------------
        // The species arrays, filled the way Graphic::PackCloudParams fills them
        // ------------------------------------------------------------------------------------------
        //
        // THE PACKER ITSELF IS NOT REACHABLE FROM HERE — CloudPayload.hpp pulls in the component, the
        // reflection macros and the atmosphere, none of which this GPU-free suite links — so what is
        // shared instead is the PURE HALF of it: Graphic::CloudSpeciesPlacementBasis and the two
        // per-species products. Those are the parts that can be wrong; the rest of the packer is a
        // transcription that ComponentReflection drives directly against the real function.
        void CloudBindSpecies( CloudFieldParams& params, const Desert::Graphic::CloudTypeShape* shapes,
                               std::uint32_t count, vec3 windDirection, float coverage = 0.35f,
                               float contrast = 1.0f )
        {
            params.SpeciesCount = static_cast<int>( count );

            for ( std::uint32_t slot = 0; slot < CLOUD_SPECIES_SLOTS; ++slot )
            {
                // EVERY SPECIES ON VOLUME 0 unless a test says otherwise, which is what
                // Graphic::ResolveCloudNoiseVolumes produces for a layer whose types name one volume
                // between them — the state of every sky in the repository. A test that wants two calls
                // CloudBindSpeciesNoise after this.
                params.SpeciesNoise[slot] = 0;

                if ( slot >= count )
                {
                    params.SpeciesEdge[slot] = vec4( 0.0f );
                    continue;
                }

                const Desert::Graphic::CloudTypeShape& shape = shapes[slot];

                params.SpeciesEdge[slot] = vec4( shape.DetailCharacter, shape.DetailFactor, shape.DensityFactor,
                                                 shape.ExtinctionFactor );
            }

            // THE PLACEMENT BASIS IS NOT BOUND HERE BECAUSE IT NO LONGER EXISTS. It told the march how to
            // read the coverage noise in each species' own frame; the lumps are placed on a lattice in
            // that frame at BAKE time now, so what the march is handed is where the volume is instead.
            CloudModellingVolumeSelectSet( shapes, count, coverage, contrast, windDirection );

            params.RegionOriginKm  = ModellingVolume().OriginKm;
            params.InvRegionSizeKm = 1.0f / ModellingVolume().Params.RegionSizeKm;
        }

        /**
         * Give species @p species its OWN noise volume, with @p periods, on a slot of its own.
         *
         * The two halves of what the renderer does, together, because they are one decision: the volume
         * goes into a bound slot and the species is pointed at it. Splitting them is exactly how a test
         * ends up asserting that a volume nothing reads was bound.
         */
        void CloudBindSpeciesNoise( CloudFieldParams& params, int species, int slot,
                                    const BoundVolumePeriods& periods )
        {
            CloudNoiseVolumeSelect( slot, periods );
            params.SpeciesNoise[species] = slot;
        }

    } // namespace
} // namespace Desert::Tests::CloudFieldRef
