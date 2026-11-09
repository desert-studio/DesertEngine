// DesertAsset {"Kind":"Shader","Guid":"ad572b5fc8784311a2fcc5a2d28adc9a","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 4/4 — INTEGRATE (UE ScreenProbeIntegrateCS, LumenScreenProbeGather.usf): one thread per
// G-buffer pixel; the temporal filter that follows it is ScreenProbeTemporal.
//   probes    the four uniform probes around the pixel, bilinear x how well each lies on the pixel's plane and
//             faces its normal; a corner that misses takes the best adaptive probe of its tile (SPInterpolate, UE
//             CalculateUpsampleInterpolationWeights). The placement put an adaptive probe wherever this test
//             failed, so only a pixel past the adaptive budget finds none: it is listed, and the uncovered
//             pass (SP_INTEGRATE_UNCOVERED, THE UNCOVERED PIXELS) reads the world radiance cache for it;
//   diffuse   the probes' irradiance SH at the pixel's bent normal times its short-range AO (already / pi; UE
//             ScreenProbeIntegrateCS with SHORT_RANGE_AO, the ScreenProbeShortRangeAO pass of this frame): the
//             occluders closer than the probes, which the scalar SSAO misses on thin leaf cards;
//   specular  UE's rough specular (LumenScreenProbeGather.usf ScreenProbeIntegrateCS, the default
//             ROUGH_SPECULAR_ALONG_REFLECTION_DIRECTION 0): the diffuse irradiance above (UE `DiffuseLighting / PI`)
//             lerped toward four visible-GGX samples of the probes' filtered radiance by RoughReflectionsDiffuseLerp
//             (fully the irradiance from roughness MaxRoughnessToEvaluateRoughSpecular = 0.8 up, fully the GGX
//             samples 0.2 below it). It is NOT the probes' radiance along the one mirror direction, which UE turns
//             off because it "samples values below the visible horizon": a direction under the probe's hemisphere
//             reads its horizon texel, the open sky at a crown's edge, and that was every pale fleck on Bistro's
//             bush and every pale-blue linde leaf (FOLIAGE-LOOK-3: the specular knockout removed both, the reflection
//             environment in its place made them brighter). The rough reflections of UE (ScreenProbeGather
//             integrates the specular of surfaces above r.Lumen.Reflections.MaxRoughnessToTrace from the same
//             probes); smoother surfaces read it too until a reflection tracer exists, so no surface reads the sky
//             cube in a closed room. Every GGX sample is weighted by its visibility in the bent cone (UE
//             CalculateSpecularOcclusion with the sample's direction as the reflection), so the short-range AO
//             occludes the rough specular direction by direction, and the irradiance it lerps toward is already
//             occluded;
//   backface  the irradiance (/ pi) from behind the pixel's surface, -N, interpolated from the probes' backface
//             (ScreenProbeFilter) under the same weights, the radiance cache's at -N where no probe holds the pixel:
//             the composite lets it through a two-sided leaf times its SubsurfaceColor (UE
//             TwoSidedFoliageBackfaceDiffuse, DiffuseIndirectComposite's BackfaceDiffuseIndirect);
// Output: diffuse.rgb + .a = the pixel's distance to the camera, specular.rgb, backface.rgb.
Shader "ScreenProbeIntegrate"
{
    Compute
    {
        LocalSize(8, 8, 1);
        #define SP_INTEGRATE_GROUP_THREADS 64u // the LocalSize above

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/MonteCarlo.glslh>

        // UE r.Lumen.ScreenProbeGather.MaxRoughnessToEvaluateRoughSpecular (and ...ForFoliage), both 0.8, and the
        // 0.2 fade of RoughReflectionsDiffuseLerp (LumenScreenProbeTileClassication.ush): at and above 0.8 the
        // rough specular IS the irradiance, below 0.6 it is all GGX samples.
        #define SP_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR 0.8
        #define SP_ROUGH_SPECULAR_FADE_LENGTH 0.2
        // UE NumSpecularSamples, and the floor UE puts under the roughness the probes can answer (a ~2x2
        // footprint).
        #define SP_ROUGH_SPECULAR_SAMPLES 4
        #define SP_ROUGH_SPECULAR_MIN_ROUGHNESS 0.2

        // UE RoughReflectionsDiffuseLerp: the share of the rough specular the irradiance answers.
        float SPRoughSpecularDiffuseLerp( float roughness )
        {
            const float start = SP_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR - SP_ROUGH_SPECULAR_FADE_LENGTH;
            return clamp( ( roughness - start ) / SP_ROUGH_SPECULAR_FADE_LENGTH, 0.0, 1.0 );
        }

        // UE ApproximateConeConeIntersection (DistanceFieldAOShared.ush): the share of cone 0 inside cone 1, both given
        // by their angles, @p between apart.
        float SPApproximateConeConeIntersection( float arc0, float arc1, float between )
        {
            const float difference = abs( arc0 - arc1 );
            return smoothstep( 0.0, 1.0,
                               1.0 - clamp( ( between - difference ) / max( arc0 + arc1 - difference, 1e-4 ), 0.0, 1.0 ) );
        }

        // UE CalculateSpecularOcclusion (LumenScreenProbeGather.usf, SHORT_RANGE_AO): how much of a reflection cone
        // of @p roughness around @p direction the bent normal's unoccluded cone (AO x pi) holds.
        float SPSpecularOcclusion( vec3 bentNormal, float ao, float roughness, vec3 direction )
        {
            const float reflectionCone  = max( roughness, 0.1 ) * 3.14159265;
            const float unoccludedAngle = ao * 3.14159265;
            const float between         = acos( clamp( dot( bentNormal, direction ), -1.0, 1.0 ) );
            const float occlusion = SPApproximateConeConeIntersection( reflectionCone, unoccludedAngle, between );
            return mix( 0.0, occlusion, clamp( ( unoccludedAngle - 0.1 ) / 0.2, 0.0, 1.0 ) );
        }

        // THE UNCOVERED PIXELS (UE Lumen: the pixels no screen probe covers are shaded apart, not by every lane of the
        // integrate): a pixel no probe holds (the placement wanted an adaptive probe there and the budget was spent)
        // sets its bit in its tile's mask, and the first such pixel of a tile lists the tile; the
        // SP_INTEGRATE_UNCOVERED permutation, one group a listed tile (indirect, the same 2D grid as the
        // classification's lists), reads the world radiance cache for the masked pixels alone and writes their three
        // outputs. The integrate kernels carry no radiance-cache read (its two 8-corner lookups, their registers),
        // and no wave of them waits on one uncovered pixel. The uncovered pass loads the tile's short-range AO texels
        // as the integrate does, so its bent normal and AO are the same values.
        //   ScreenProbeIntegrateUncoveredArgsUB  x, y, z, count (uploaded 0, 0, 1, 0);
        //   ScreenProbeIntegrateUncoveredUB      the listed tiles (x | y << 16);
        //   ScreenProbeIntegrateUncoveredMaskUB  per tile (x + y * SP_INTEGRATE_TILES_X) 3 words: the 64 pixels'
        //                                        bits (local index), then the listed flag; zeroed by the
        //                                        classification, which runs before either integrate.
        #define SP_UNCOVERED_MASK_WORDS 3u

        // INTEGRATION TILE CLASSIFICATION (UE r.Lumen.ScreenProbeGather.IntegrationTileClassification,
        // ScreenProbeTileClassificationMarkCS + BuildListsCS in one pass): an 8x8 tile
        // none of whose pixels evaluates GGX samples (every one at or above the rough-specular cut, or the sky) is
        // SIMPLE DIFFUSE, integrated by the SP_INTEGRATE_SIMPLE_DIFFUSE permutation that has no specular sampling
        // compiled in (its registers, its occupancy); every other tile takes the full permutation. Both dispatch
        // indirectly over their list. ScreenProbeIntegrateTilesUB: the simple tiles from the front, the full ones
        // from the back (x | y << 16 each); ScreenProbeIntegrateArgsUB: the simple dispatch, then the full one,
        // each x, y, z, count (uploaded as 0, 0, 1, 0). A list's dispatch is 2D, SP_INTEGRATE_GROUPS_X groups a
        // row: a 4K frame's 129 600 tiles in one row exceed the 65 535 groups a dimension Vulkan guarantees
        // (maxComputeWorkGroupCount, VUID-VkDispatchIndirectCommand-x-00417). The last row's groups past the
        // count (w) return at once.
        #define SP_INTEGRATE_GROUPS_X 1024u
        #define SP_INTEGRATE_TILES_X ( ( uint( giFrame.screen.x ) + 7u ) / 8u )
        #define SP_INTEGRATE_TILE_COUNT ( SP_INTEGRATE_TILES_X * ( ( uint( giFrame.screen.y ) + 7u ) / 8u ) )
#if defined( SP_INTEGRATE_CLASSIFY )
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler PointClampSampler;
        Uniform(7) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        Buffer(22) ScreenProbeIntegrateArgsUB
        {
            uint screenProbeIntegrateArgs[];
        };
        Buffer(23) ScreenProbeIntegrateTilesUB
        {
            uint screenProbeIntegrateTiles[];
        };
        Buffer(26) ScreenProbeIntegrateUncoveredMaskUB
        {
            uint screenProbeIntegrateUncoveredMask[]; // THE UNCOVERED PIXELS above: zeroed here, per tile
        };

        shared uint s_EvaluatesRoughSpecular;

        void main()
        {
            if ( gl_LocalInvocationIndex == 0u )
                s_EvaluatesRoughSpecular = 0u;
            barrier();
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            if ( all( lessThan( pixel, ivec2( giFrame.screen.xy ) ) ) )
            {
                const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
                if ( dot( gb.xyz, gb.xyz ) > 0.001 && SPRoughSpecularDiffuseLerp( max( gb.a, 0.04 ) ) < 1.0 )
                    atomicOr( s_EvaluatesRoughSpecular, 1u );
            }
            barrier();
            const uint maskBase =
                 ( gl_WorkGroupID.y * SP_INTEGRATE_TILES_X + gl_WorkGroupID.x ) * SP_UNCOVERED_MASK_WORDS;
            if ( gl_LocalInvocationIndex < SP_UNCOVERED_MASK_WORDS )
                screenProbeIntegrateUncoveredMask[maskBase + gl_LocalInvocationIndex] = 0u;
            if ( gl_LocalInvocationIndex != 0u )
                return;
            const uint full = s_EvaluatesRoughSpecular;
            const uint args = full * 4u;
            const uint slot = atomicAdd( screenProbeIntegrateArgs[args + 3u], 1u );
            // The 2D grid that covers slot: as wide as the list up to a row, as many rows as it fills.
            atomicMax( screenProbeIntegrateArgs[args], min( slot + 1u, SP_INTEGRATE_GROUPS_X ) );
            atomicMax( screenProbeIntegrateArgs[args + 1u], slot / SP_INTEGRATE_GROUPS_X + 1u );
            const uint tile = gl_WorkGroupID.x | ( gl_WorkGroupID.y << 16u );
            if ( full != 0u )
                screenProbeIntegrateTiles[SP_INTEGRATE_TILE_COUNT - 1u - slot] = tile;
            else
                screenProbeIntegrateTiles[slot] = tile;
        }
#else
        // The integrate permutations (simple diffuse, full) and the uncovered-pixel pass (SP_INTEGRATE_UNCOVERED):
        // the G-buffer, the short-range AO and the three outputs are common; the probes are the integrate's, the
        // world radiance cache the uncovered pass's (THE UNCOVERED PIXELS above).
        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
#if defined( SP_INTEGRATE_UNCOVERED ) || !defined( SP_INTEGRATE_SIMPLE_DIFFUSE )
        Uniform sampler LinearClampSampler; // sampled by the cache (UNCOVERED) or the filtered probes only
#endif
        Uniform sampler PointClampSampler;
        Uniform(6) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(7) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        layout(binding = 10, rgba16f) restrict writeonly uniform image2D ScreenProbeDiffuseOutTexture;
        layout(binding = 11, rgba16f) restrict writeonly uniform image2D ScreenProbeSpecularOutTexture;
        layout(binding = 19, rgba16f) restrict writeonly uniform image2D ScreenProbeBackfaceOutTexture;
        Uniform(20) texture2D ShortRangeAoTexture; // half-res, oct world bent normal + AO (ScreenProbes.glslh)
        #define ShortRangeAoTexture sampler2D(ShortRangeAoTexture, PointClampSampler)
#if defined( SP_INTEGRATE_UNCOVERED )
        ReadBuffer(12) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(13) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(14) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(15) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(16) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        ReadBuffer(24) ScreenProbeIntegrateUncoveredArgsUB
        {
            uint screenProbeIntegrateUncoveredArgs[]; // x, y, z, count (THE UNCOVERED PIXELS above)
        };
        ReadBuffer(25) ScreenProbeIntegrateUncoveredUB
        {
            uint screenProbeIntegrateUncovered[]; // the listed tiles
        };
        ReadBuffer(26) ScreenProbeIntegrateUncoveredMaskUB
        {
            uint screenProbeIntegrateUncoveredMask[]; // per tile, SP_UNCOVERED_MASK_WORDS
        };
#else
        ReadBuffer(1) ScreenProbeIndexUB
        {
            uint screenProbeIndex[];
        };
        ReadBuffer(2) ScreenProbesUB
        {
            ScreenProbe screenProbes[];
        };
        ReadBuffer(4) ScreenProbeSHUB
        {
            vec4 screenProbeSH[]; // SP_SH_PACKED_VEC4 per probe (ScreenProbes.glslh)
        };
#if !defined( SP_INTEGRATE_SIMPLE_DIFFUSE )
        Uniform(5) texture2D ScreenProbeFilteredTexture; // the GGX samples' radiance: the full permutation only
        #define ScreenProbeFilteredTexture sampler2D(ScreenProbeFilteredTexture, LinearClampSampler)
#endif
        ReadBuffer(17) ScreenProbeBackfaceUB
        {
            vec4 screenProbeBackface[];
        };
        ReadBuffer(21) ScreenProbeMovingUB
        {
            float screenProbeMoving[]; // per probe, the share of its rays that are moving (ScreenProbeFilter)
        };
        ReadBuffer(22) ScreenProbeIntegrateArgsUB
        {
            uint screenProbeIntegrateArgs[]; // the two dispatches' x, y, z, count (the classification above)
        };
        ReadBuffer(23) ScreenProbeIntegrateTilesUB
        {
            uint screenProbeIntegrateTiles[]; // this permutation's tiles (the classification above)
        };
        Buffer(24) ScreenProbeIntegrateUncoveredArgsUB
        {
            uint screenProbeIntegrateUncoveredArgs[]; // x, y, z, count (THE UNCOVERED PIXELS above)
        };
        Buffer(25) ScreenProbeIntegrateUncoveredUB
        {
            uint screenProbeIntegrateUncovered[]; // the listed tiles
        };
        Buffer(26) ScreenProbeIntegrateUncoveredMaskUB
        {
            uint screenProbeIntegrateUncoveredMask[]; // per tile, SP_UNCOVERED_MASK_WORDS
        };
#endif

        // THE TILE'S PROBE NEIGHBOURHOOD, LOADED ONCE (SPInterpolate's SP_INTERPOLATE_NEIGHBOURHOOD,
        // ScreenProbes.glslh): an 8x8 tile's pixels interpolate the uniform probes of at most 3x3 uniform tiles
        // (8 pixels span at most two of the jittered SP_DOWNSAMPLE-pixel cells per axis, plus the corner after),
        // and each failed corner scans its tile's adaptive probes. Every pixel read the uniform tile's index
        // entry, then the 32-byte probe, per corner, and two dependent loads per adaptive candidate: up to
        // 4 x SP_ADAPTIVE_PER_TILE (60 at 16 px) on the foliage, where the corners miss. One thread a (tile, slot)
        // loads the ids, counts and probes into groupshared memory here, so the scan reads no buffer. The same
        // values in the same order: the picture is unchanged. 9 x (1 + SP_ADAPTIVE_PER_TILE) x 36 bytes: 5.2 KB at 16 px,
        // 20.7 KB at 32; a 64-pixel layout (83 KB, past the 48 KB a group may hold) reads the buffers as before.
#if SP_DOWNSAMPLE_LOG2 <= 5 && !defined( SP_INTEGRATE_UNCOVERED )
        #define SP_INTERPOLATE_NEIGHBOURHOOD
        #define SP_NB_SIDE 3
        #define SP_NB_TILES ( SP_NB_SIDE * SP_NB_SIDE )
        #define SP_NB_SLOTS ( 1u + SP_ADAPTIVE_PER_TILE )
        #define SP_NB_ENTRIES ( uint( SP_NB_TILES ) * SP_NB_SLOTS )
        #define SP_NB_COUNTS ( uint( SP_NB_TILES ) * ( SP_LEVELS - 1u ) )
        shared uint s_NbId[SP_NB_ENTRIES];
        shared vec4 s_NbPosition[SP_NB_ENTRIES];
        shared vec4 s_NbNormal[SP_NB_ENTRIES];
        shared uint s_NbCount[SP_NB_COUNTS];
        // The neighbourhood's first uniform tile (SPNeighbourhoodBase): the same for every thread of the group.
        ivec2 g_NbBase;

        uint SPNeighbourTile( ivec2 tile )
        {
            const ivec2 local = tile - g_NbBase;
            return uint( local.y * SP_NB_SIDE + local.x );
        }

        uint SPNeighbourUniform( ivec2 tile )
        {
            return s_NbId[SPNeighbourTile( tile ) * SP_NB_SLOTS];
        }

        uint SPNeighbourCount( ivec2 tile, uint level )
        {
            return s_NbCount[SPNeighbourTile( tile ) * ( SP_LEVELS - 1u ) + level - 1u];
        }

        uint SPNeighbourAdaptive( ivec2 tile, uint slot )
        {
            return s_NbId[SPNeighbourTile( tile ) * SP_NB_SLOTS + slot];
        }

        ScreenProbe SPNeighbourProbe( ivec2 tile, uint slot, uint id )
        {
            const uint  entry = SPNeighbourTile( tile ) * SP_NB_SLOTS + slot;
            ScreenProbe probe;
            probe.position = s_NbPosition[entry];
            probe.normal   = s_NbNormal[entry];
            return probe;
        }
#endif
#if defined( SP_INTEGRATE_UNCOVERED )
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>
#else
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
#endif

        // The short-range AO at @p pixel (world position @p p, @p distance from the camera, shading normal @p n)
        // from its half-resolution target (UE ShortRangeAO.Downsample's upsample in the integrate): the four
        // texels around the pixel, each the AO of the G-buffer pixel it traced this frame, under bilinear weights
        // (by where each traced) times
        //   a PLANE weight - how far the traced pixel lies off this pixel's tangent plane, relative to the
        //     distance: a crown before a wall, a post before the field take nothing of the far side, and the
        //     neighbours on a floor seen at a grazing angle (far apart in depth, all on one plane) all count;
        //   a NORMAL weight - at a wall-floor seam the floor's texel is not the wall's.
        // Each texel's world-space bent normal is carried from its traced pixel's normal onto this one's by the
        // shortest arc (the normal weight keeps the two within a hemisphere), so the full-resolution normal
        // detail stays and no tangent frame enters. No texel of this surface (thin geometry a pixel wide on the
        // frames that trace its neighbour): unoccluded about this pixel's own normal, never another surface's
        // AO; the frame that traces the pixel itself always matches (its own texel, weight 1), and the temporal
        // filter holds it.
        #define SP_SHORT_RANGE_AO_PLANE_TOLERANCE 0.03 // the plane distance, relative, that halves a weight
        // The normal weight: max(dot(n_traced, n), 0) ^ 8, as three squarings (a pow is an exp2 of a log2).
        float SPShortRangeAONormalWeight( float facing )
        {
            const float f2 = facing * facing;
            const float f4 = f2 * f2;
            return f4 * f4;
        }
        // THE TILE'S TRACED TEXELS, LOADED ONCE (UE ScreenProbeIntegrateCS reads its G-buffer once a pixel): an 8x8
        // tile's pixels upsample from the SP_SR_AO_TILE^2 texels starting at (tile origin - offset) / 2, each with
        // its traced pixel's normal and world position. One thread a texel fetches the AO texel, the traced
        // pixel's normal and depth and reconstructs its position into shared memory, so a pixel's four texels
        // cost no G-buffer fetch and no matrix product (they cost 4 AO + 4 normal + 4 depth fetches and 4 mat4
        // products a pixel before). The same values in the same sums: the picture is unchanged.
        //   x  = the AO texel (w <= 0: no surface; the normal and position are then not loaded);
        //   n  = the traced pixel's normalised G-buffer normal;   p = its world position.
        #define SP_SR_AO_TILE 5
        #define SP_SR_AO_TEXELS ( SP_SR_AO_TILE * SP_SR_AO_TILE )
        shared vec4 s_SRAoSample[SP_SR_AO_TEXELS];
        shared vec3 s_SRAoNormal[SP_SR_AO_TEXELS];
        shared vec3 s_SRAoPosition[SP_SR_AO_TEXELS];

        // The first texel a tile at @p tileOrigin (pixels) upsamples from: floor((origin - offset) / 2), the origin
        // even, the offset 0 or 1. Its pixels' texels at a non-zero bilinear weight span 5 from there; with the
        // offset 1 the tile's last pixel column/row lands on a texel exactly (f = 0) and names a 6th at weight 0,
        // which SPShortRangeAOAt skips before indexing.
        ivec2 SPShortRangeAOTileBase( ivec2 tileOrigin )
        {
            return tileOrigin / 2 - SPShortRangeAOOffset( screenProbeParams.atlas.y );
        }

        // Called by every thread of the group in uniform control flow (it holds the barrier).
        void SPShortRangeAOLoadTile( ivec2 tileOrigin, bool enabled )
        {
            const uint index = gl_LocalInvocationIndex;
            if ( enabled && index < uint( SP_SR_AO_TEXELS ) )
            {
                const ivec2 extent   = ivec2( giFrame.screen.xy );
                const ivec2 aoExtent = ( extent + 1 ) / 2;
                const ivec2 offset   = SPShortRangeAOOffset( screenProbeParams.atlas.y );
                const ivec2 local    = ivec2( int( index ) % SP_SR_AO_TILE, int( index ) / SP_SR_AO_TILE );
                const ivec2 texel = clamp( SPShortRangeAOTileBase( tileOrigin ) + local, ivec2( 0 ), aoExtent - 1 );
                const vec4  s     = texelFetch( ShortRangeAoTexture, texel, 0 );
                vec3        nt    = vec3( 0.0 );
                vec3        pt    = vec3( 0.0 );
                if ( s.w > 0.0 )
                {
                    const ivec2 traced = min( texel * 2 + offset, extent - 1 );
                    const vec2  uvt    = ( vec2( traced ) + 0.5 ) * giFrame.screen.zw;
                    nt = normalize( texelFetch( GBufferBTexture, traced, 0 ).xyz );
                    pt = ReconstructWorldPosition( uvt, texelFetch( GBufferDepthTexture, traced, 0 ).r,
                                                   giFrame.invViewProjection );
                }
                s_SRAoSample[index]   = s;
                s_SRAoNormal[index]   = nt;
                s_SRAoPosition[index] = pt;
            }
            barrier();
        }

#if defined( SP_INTERPOLATE_NEIGHBOURHOOD )
        // The neighbourhood's first uniform tile: SPInterpolate's floor((pixel - jitter) / SP_DOWNSAMPLE) of the
        // tile's first pixel; its last pixel's lies at most one further, and its corners one past that.
        ivec2 SPNeighbourhoodBase( ivec2 tileOrigin )
        {
            return ivec2( floor( vec2( tileOrigin - ivec2( SPJitter() ) ) / float( SP_DOWNSAMPLE ) ) );
        }

        // Fills the neighbourhood of the pixel tile at @p tileOrigin. Called by every thread before the barrier of
        // SPShortRangeAOLoadTile, which publishes it. A slot past its level's count keeps SP_INVALID and loads no
        // probe (its index entry is a previous frame's, or never written).
        void SPNeighbourhoodLoad( ivec2 tileOrigin )
        {
            g_NbBase = SPNeighbourhoodBase( tileOrigin );
            for ( uint e = gl_LocalInvocationIndex; e < SP_NB_COUNTS; e += SP_INTEGRATE_GROUP_THREADS )
            {
                const uint  local = e / ( SP_LEVELS - 1u );
                const ivec2 tile  = g_NbBase + ivec2( int( local ) % SP_NB_SIDE, int( local ) / SP_NB_SIDE );
                const uint  level = e % ( SP_LEVELS - 1u ) + 1u;
                uint        count = 0u;
                if ( SPUniformTileInside( tile ) )
                    count = min( screenProbeIndex[SPTileHeader( SPUniformTileIndex( tile ), level )],
                                 SPLevelSlots( level ) );
                s_NbCount[e] = count;
            }
            for ( uint e = gl_LocalInvocationIndex; e < SP_NB_ENTRIES; e += SP_INTEGRATE_GROUP_THREADS )
            {
                const uint  local = e / SP_NB_SLOTS;
                const uint  slot  = e % SP_NB_SLOTS;
                const ivec2 tile  = g_NbBase + ivec2( int( local ) % SP_NB_SIDE, int( local ) / SP_NB_SIDE );
                uint        id    = SP_INVALID;
                if ( slot == 0u )
                    id = SPUniformProbeAt( tile );
                else if ( SPUniformTileInside( tile ) )
                {
                    // The slot's level and its k inside it (SPLevelSlotBase( level ) + k = slot - 1).
                    uint level = 1u;
                    while ( level + 1u < SP_LEVELS && slot - 1u >= SPLevelSlotBase( level + 1u ) )
                        ++level;
                    const uint t     = SPUniformTileIndex( tile );
                    const uint count = min( screenProbeIndex[SPTileHeader( t, level )], SPLevelSlots( level ) );
                    if ( slot - 1u - SPLevelSlotBase( level ) < count )
                        id = screenProbeIndex[SPTileSlot( t, level, slot - 1u - SPLevelSlotBase( level ) )];
                }
                vec4 position = vec4( 0.0 );
                vec4 normal   = vec4( 0.0 );
                if ( id != SP_INVALID )
                {
                    position = screenProbes[id].position;
                    normal   = screenProbes[id].normal;
                }
                s_NbId[e]       = id;
                s_NbPosition[e] = position;
                s_NbNormal[e]   = normal;
            }
        }
#endif

        // @p tileOrigin: the pixel tile SPShortRangeAOLoadTile loaded.
        void SPShortRangeAOAt( ivec2 pixel, ivec2 tileOrigin, vec3 p, float distance, vec3 n, out vec3 bent,
                               out float ao )
        {
            const ivec2 offset    = SPShortRangeAOOffset( screenProbeParams.atlas.y );
            const vec2  c         = vec2( pixel - offset ) * 0.5;
            const ivec2 b         = ivec2( floor( c ) );
            const vec2  f         = c - vec2( b );
            const ivec2 local     = b - SPShortRangeAOTileBase( tileOrigin );
            const float tolerance = distance * SP_SHORT_RANGE_AO_PLANE_TOLERANCE;
            vec3        bentSum   = vec3( 0.0 );
            float       aoSum     = 0.0;
            float       total     = 0.0;
            for ( int i = 0; i < 4; ++i )
            {
                const vec2 bilinear = mix( 1.0 - f, f, vec2( i & 1, i >> 1 ) );
                // Weight 0 first: it is the only case whose texel lies past the loaded SP_SR_AO_TILE.
                if ( bilinear.x * bilinear.y <= 0.0 )
                    continue;
                const ivec2 at   = local + ivec2( i & 1, i >> 1 );
                const int   slot = at.y * SP_SR_AO_TILE + at.x;
                const vec4  s    = s_SRAoSample[slot];
                if ( s.w <= 0.0 )
                    continue;
                const vec3  nt     = s_SRAoNormal[slot];
                const float facing = dot( nt, n );
                if ( facing <= 0.0 )
                    continue;
                const vec3  pt    = s_SRAoPosition[slot];
                const float plane = abs( dot( pt - p, n ) ) / tolerance;
                const float w     = bilinear.x * bilinear.y * exp2( -plane * plane ) *
                                    SPShortRangeAONormalWeight( facing );
                // Rodrigues' rotation by the shortest arc nt -> n (axis nt x n, cosine facing > 0).
                const vec3 bt   = SPOctDecode( s.xy );
                const vec3 axis = cross( nt, n );
                bentSum += w * ( bt * facing + cross( axis, bt ) + axis * ( dot( axis, bt ) / ( 1.0 + facing ) ) );
                aoSum += w * s.z;
                total += w;
            }
            bent = n;
            ao   = 1.0;
            if ( total > 1e-3 )
            {
                if ( dot( bentSum, bentSum ) > 1e-8 )
                    bent = normalize( bentSum );
                ao = aoSum / total;
            }
        }

#if !defined( SP_INTEGRATE_UNCOVERED )
        // Per pixel, sample and frame (PCG, as ScreenProbeTrace's TexelNoise).
        vec2 SPPixelNoise( uvec2 pixel, uint index, uint frame )
        {
            uvec3 h = uvec3( pixel.x, pixel.y * 4u + index, frame ) * 1664525u + 1013904223u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            h ^= h >> 16u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            return vec2( h.xy & 0xFFFFu ) / 65536.0;
        }

        // UE MonteCarlo.ush ImportanceSampleVisibleGGX (Dupuy & Benyoub 2023, spherical caps; isotropic, unbounded):
        // a micro-normal of the GGX lobe of `alpha` seen from tangent-space `v` (z = the shading normal).
        vec3 SPSampleVisibleGGX( vec2 e, float alpha, vec3 v )
        {
            const vec3  vh  = normalize( vec3( alpha * v.xy, v.z ) );
            const float phi = 6.28318530718 * e.x;
            const float z   = mix( 1.0, -vh.z, e.y );
            const float st  = sqrt( clamp( 1.0 - z * z, 0.0, 1.0 ) );
            const vec3  h   = vec3( st * cos( phi ), st * sin( phi ), z ) + vh;
            return normalize( vec3( alpha * h.xy, max( 0.0, h.z ) ) );
        }

        // The probes a pixel interpolates: four per level.
        #define SP_PIXEL_PROBES ( 4u * SP_LEVELS )

        // The irradiance of @p probe at the direction whose SH basis is @p basis (SPSHBasisAll, once per pixel):
        // seven vec4 loads of the packed coefficients (SP_SH_PACKED_VEC4) and one dot, the same sum in the same
        // order as nine per-coefficient evaluations. Each probe's irradiance is clamped before the blend, as before:
        // UE blends the coefficients and clamps once, the same multiply-adds for 27 more live registers, and a
        // probe ringing below zero would then pull its neighbours down.
        vec3 ProbeIrradiance( uint probe, float basis[9] )
        {
            const uint base = probe * SP_SH_PACKED_VEC4;
            const vec4 s0   = screenProbeSH[base];
            const vec4 s1   = screenProbeSH[base + 1u];
            const vec4 s2   = screenProbeSH[base + 2u];
            const vec4 s3   = screenProbeSH[base + 3u];
            const vec4 s4   = screenProbeSH[base + 4u];
            const vec4 s5   = screenProbeSH[base + 5u];
            const vec4 s6   = screenProbeSH[base + 6u];
            vec3       e    = s0.xyz * basis[0];
            e += vec3( s0.w, s1.xy ) * basis[1];
            e += vec3( s1.zw, s2.x ) * basis[2];
            e += s2.yzw * basis[3];
            e += s3.xyz * basis[4];
            e += vec3( s3.w, s4.xy ) * basis[5];
            e += vec3( s4.zw, s5.x ) * basis[6];
            e += s5.yzw * basis[7];
            e += s6.xyz * basis[8];
            return max( e, vec3( 0.0 ) );
        }

#if !defined( SP_INTEGRATE_SIMPLE_DIFFUSE )
        vec3 ProbeRadiance( uint probe, vec3 direction )
        {
            const ScreenProbe state = screenProbes[probe];
            const vec2 o      = SPProbeOct( SPProbeFrame( state.normal.xyz ), direction );
            const vec2 texel  = vec2( SPAtlasOrigin( probe, SP_TILE ) + 1 ) + ( o * 0.5 + 0.5 ) * float( SP_RES );
            return textureLod( ScreenProbeFilteredTexture, texel / vec2( textureSize( ScreenProbeFilteredTexture, 0 ) ), 0.0 ).rgb;
        }
#endif

        void main()
        {
            // One workgroup per tile of this permutation's list (the classification): the simple tiles from the
            // front, the full ones from the back; the 2D grid's groups past the list's count do nothing.
            const uint listIndex = gl_WorkGroupID.y * SP_INTEGRATE_GROUPS_X + gl_WorkGroupID.x;
#if defined( SP_INTEGRATE_SIMPLE_DIFFUSE )
            if ( listIndex >= screenProbeIntegrateArgs[3u] )
                return;
            const uint  tile   = screenProbeIntegrateTiles[listIndex];
#else
            if ( listIndex >= screenProbeIntegrateArgs[7u] )
                return;
            const uint  tile   = screenProbeIntegrateTiles[SP_INTEGRATE_TILE_COUNT - 1u - listIndex];
#endif
            const ivec2 tileOrigin = ivec2( tile & 0xFFFFu, tile >> 16u ) * 8;
            const ivec2 pixel      = tileOrigin + ivec2( gl_LocalInvocationID.xy );
            const vec2  extent     = giFrame.screen.xy;
            // Off (Scalability GlobalIllumination.ShortRangeAO 0): the shading normal, unoccluded; SSAO occludes the
            // gather in the composite instead. The tile's AO texels load before any pixel returns: the whole group
            // reaches the barrier (the flag and the tile are uniform over it).
            const bool shortRangeAO = ( screenProbeParams.atlas.z & SP_FLAG_SHORT_RANGE_AO ) != 0u;
#if defined( SP_INTERPOLATE_NEIGHBOURHOOD )
            // Published by the barrier in SPShortRangeAOLoadTile, which every thread reaches whatever the flag.
            SPNeighbourhoodLoad( tileOrigin );
#endif
            SPShortRangeAOLoadTile( tileOrigin, shortRangeAO );
            if ( any( greaterThanEqual( pixel, ivec2( extent ) ) ) )
                return;
            const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( ScreenProbeDiffuseOutTexture, pixel, vec4( 0.0 ) );
                imageStore( ScreenProbeSpecularOutTexture, pixel, vec4( 0.0 ) );
                imageStore( ScreenProbeBackfaceOutTexture, pixel, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( p - giFrame.camera.xyz ), 1e-3 );
            const vec3  v        = ( giFrame.camera.xyz - p ) / distance;
            vec3  bent = n;
            float ao   = 1.0;
            if ( shortRangeAO )
                SPShortRangeAOAt( pixel, tileOrigin, p, distance, n, bent, ao );
            const float roughness   = max( gb.a, 0.04 );
            const float diffuseLerp = SPRoughSpecularDiffuseLerp( roughness );

            vec3  diffuse  = vec3( 0.0 );
            vec3  specular = vec3( 0.0 );
            vec3  backface = vec3( 0.0 );
            // UE LightIsMoving (LumenScreenProbeGather.usf:1450-1454): the held probes' moving shares under the
            // diffuse's weights; 0 where no probe holds the pixel (UE: lighting not valid). It rides in the rough
            // specular's alpha, which nothing else reads, to the temporal filter's fast update.
            float lightIsMoving = 0.0;
            // The probes that hold the pixel and their weights: the diffuse reads every one, each GGX sample one of
            // them picked by weight (UE r.Lumen.ScreenProbeGather.StochasticInterpolation) - four radiance fetches a
            // pixel instead of four per probe (Bistro's backlit crown: 7.1 ms of integrate, then a GPU watchdog).
            uvec4 heldId;
            vec4  heldW;
            SPInterpolate( pixel, p, n, distance, SP_LEVELS, heldId, heldW );
            const float total = dot( heldW, vec4( 1.0 ) );
            if ( total >= SP_ADAPTIVE_EPSILON )
            {
                float basis[9];
                SPSHBasisAll( bent, basis );
                for ( int i = 0; i < 4; ++i )
                    if ( heldW[i] > 0.0 )
                        diffuse += heldW[i] * ProbeIrradiance( heldId[i], basis );
                diffuse *= ao / total;
                for ( int i = 0; i < 4; ++i )
                    if ( heldW[i] > 0.0 )
                        lightIsMoving += heldW[i] * screenProbeMoving[heldId[i]];
                lightIsMoving /= total;
                float backTotal = 0.0;
                for ( int i = 0; i < 4; ++i )
                    if ( heldW[i] > 0.0 )
                    {
                        const vec4 back = screenProbeBackface[heldId[i]];
                        backface += heldW[i] * back.a * back.rgb;
                        backTotal += heldW[i] * back.a;
                    }
                backface /= max( backTotal, 1e-6 );
#if defined( SP_INTEGRATE_SIMPLE_DIFFUSE )
                // A simple-diffuse tile: every pixel's rough specular IS its irradiance (diffuseLerp 1).
                specular = diffuse;
#else
                // UE's rough specular samples (ScreenProbeIntegrateCS, Rough-Specular): four VNDF reflections
                // of the view about the shading normal, generated here, after the diffuse, so none of them is held
                // across the interpolation. A shading normal facing away (a normal-mapped or two-sided leaf
                // seen edge-on) samples with |N.V|, UE's BxDF convention (BxDFContext NoV = saturate(abs(NoV))); a
                // sample that leaves below the surface carries nothing, as the GGX integrand's N.L does.
                if ( diffuseLerp < 1.0 )
                {
                    const mat3  frame = SPProbeFrame( n );
                    vec3        vt    = transpose( frame ) * v;
                    vt.z              = max( abs( vt.z ), 1e-4 );
                    const float alpha = max( roughness, SP_ROUGH_SPECULAR_MIN_ROUGHNESS ) *
                                        max( roughness, SP_ROUGH_SPECULAR_MIN_ROUGHNESS );
                    // UE BiasBSDFImportantSample: the second coordinate squeezed 10% toward its middle.
                    const vec2 jitter     = SPPixelNoise( uvec2( pixel ), 0u, screenProbeParams.atlas.y );
                    const uint frameIndex = screenProbeParams.atlas.y;
                    for ( int k = 0; k < SP_ROUGH_SPECULAR_SAMPLES; ++k )
                    {
                        vec2 e = vec2( fract( jitter.x + float( k ) / float( SP_ROUGH_SPECULAR_SAMPLES ) ),
                                       SPPixelNoise( uvec2( pixel ), uint( k ) + 1u, frameIndex ).y );
                        e.y           = ( e.y - 0.5 ) * 0.9 + 0.5;
                        const vec3 h  = SPSampleVisibleGGX( e, alpha, vt );
                        const vec3 lt = reflect( -vt, h );
                        if ( lt.z <= 0.0 )
                            continue;
                        const vec3  l      = frame * lt;
                        const float weight = SPSpecularOcclusion( bent, ao, roughness, l ) /
                                             float( SP_ROUGH_SPECULAR_SAMPLES );
                        if ( weight <= 0.0 )
                            continue;
                        float pick =
                             SPPixelNoise( uvec2( pixel ), uint( k ) + 8u, screenProbeParams.atlas.y ).x * total;
                        int j = 0;
                        for ( int i = 0; i < 4; ++i )
                            if ( heldW[i] > 0.0 )
                            {
                                j = i;
                                if ( pick < heldW[i] )
                                    break;
                                pick -= heldW[i];
                            }
                        specular += weight * ProbeRadiance( heldId[j], l );
                    }
                }
                specular = mix( specular, diffuse, diffuseLerp );
#endif
            }
            else
            {
                // NO SCREEN PROBE HOLDS THE PIXEL: its bit in the tile's mask; the tile's first such pixel lists the
                // tile for the uncovered pass (SP_INTEGRATE_UNCOVERED), which writes its three outputs from the
                // world radiance cache. The list's dispatch grows to cover the slot: as wide as the list up to a
                // row, as many rows as it fills.
                const ivec2 tileXY   = tileOrigin / 8;
                const uint  maskBase = ( uint( tileXY.y ) * SP_INTEGRATE_TILES_X + uint( tileXY.x ) ) *
                                       SP_UNCOVERED_MASK_WORDS;
                const uint  bit      = gl_LocalInvocationIndex;
                atomicOr( screenProbeIntegrateUncoveredMask[maskBase + bit / 32u], 1u << ( bit & 31u ) );
                if ( atomicExchange( screenProbeIntegrateUncoveredMask[maskBase + 2u], 1u ) == 0u )
                {
                    const uint slot = atomicAdd( screenProbeIntegrateUncoveredArgs[3u], 1u );
                    screenProbeIntegrateUncovered[slot] = tile;
                    atomicMax( screenProbeIntegrateUncoveredArgs[0u], min( slot + 1u, SP_INTEGRATE_GROUPS_X ) );
                    atomicMax( screenProbeIntegrateUncoveredArgs[1u], slot / SP_INTEGRATE_GROUPS_X + 1u );
                }
                return;
            }

            imageStore( ScreenProbeDiffuseOutTexture, pixel, vec4( diffuse, distance ) );
            imageStore( ScreenProbeSpecularOutTexture, pixel, vec4( specular, lightIsMoving ) );
            imageStore( ScreenProbeBackfaceOutTexture, pixel, vec4( backface, 1.0 ) );
        }
#else
        // NO SCREEN PROBE HOLDS THE PIXEL (UE would draw it dark, its weights over max(sum, Epsilon)): it reads the
        // world radiance cache, as the ProbeVolume method does for every pixel (IrradianceFieldGather) - the same
        // cache the probes' own rays end in - and is never black (FOLIAGE-LOOK-2 x1_uncovered: the black leaves).
        // One group a listed tile; its pixels outside the tile's mask do nothing.
        void main()
        {
            const uint listIndex = gl_WorkGroupID.y * SP_INTEGRATE_GROUPS_X + gl_WorkGroupID.x;
            if ( listIndex >= screenProbeIntegrateUncoveredArgs[3u] )
                return; // uniform over the group: before the barrier
            const uint  tile       = screenProbeIntegrateUncovered[listIndex];
            const ivec2 tileXY     = ivec2( tile & 0xFFFFu, tile >> 16u );
            const ivec2 tileOrigin = tileXY * 8;
            const ivec2 pixel      = tileOrigin + ivec2( gl_LocalInvocationID.xy );
            // The integrate's AO texels of this tile, the same values (every thread reaches the barrier).
            const bool shortRangeAO = ( screenProbeParams.atlas.z & SP_FLAG_SHORT_RANGE_AO ) != 0u;
            SPShortRangeAOLoadTile( tileOrigin, shortRangeAO );
            const uint maskBase =
                 ( uint( tileXY.y ) * SP_INTEGRATE_TILES_X + uint( tileXY.x ) ) * SP_UNCOVERED_MASK_WORDS;
            const uint bit = gl_LocalInvocationIndex;
            if ( ( screenProbeIntegrateUncoveredMask[maskBase + bit / 32u] & ( 1u << ( bit & 31u ) ) ) == 0u )
                return;
            const vec4  gb       = texelFetch( GBufferBTexture, pixel, 0 );
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( p - giFrame.camera.xyz ), 1e-3 );
            const vec3  v        = ( giFrame.camera.xyz - p ) / distance;
            const vec3  r        = reflect( -v, n );
            vec3  bent = n;
            float ao   = 1.0;
            if ( shortRangeAO )
                SPShortRangeAOAt( pixel, tileOrigin, p, distance, n, bent, ao );
            const float roughness   = max( gb.a, 0.04 );
            const float diffuseLerp = SPRoughSpecularDiffuseLerp( roughness );

            vec3       diffuse  = vec3( 0.0 );
            vec3       specular = vec3( 0.0 );
            vec3       backface = vec3( 0.0 );
            vec3       cacheSpecular;
            const vec4 cache = RadianceCacheIrradianceAtImpl( p, n, v, r, 0.5, true, cacheSpecular );
            if ( cache.a > 0.0 )
            {
                // The cache answers the mirror direction only: UE's lerp toward the irradiance holds here too.
                diffuse  = cache.rgb * ao;
                specular = mix( cacheSpecular * SPSpecularOcclusion( bent, ao, roughness, r ), diffuse, diffuseLerp );
            }
            const vec4 back = RadianceCacheIrradianceAt( p, -n, v, 0.5 );
            if ( back.a > 0.0 )
                backface = back.rgb;
            // The specular's alpha (lightIsMoving) 0: no probe holds the pixel (UE: lighting not valid).
            imageStore( ScreenProbeDiffuseOutTexture, pixel, vec4( diffuse, distance ) );
            imageStore( ScreenProbeSpecularOutTexture, pixel, vec4( specular, 0.0 ) );
            imageStore( ScreenProbeBackfaceOutTexture, pixel, vec4( backface, 1.0 ) );
        }
#endif // SP_INTEGRATE_UNCOVERED
#endif // SP_INTEGRATE_CLASSIFY
    }
}
