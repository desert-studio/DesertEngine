// DesertAsset {"Kind":"Shader","Guid":"a0fa2ae2b4ca49f2940bb34be1953134","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 4/5 — TRACE (UE TraceFromProbes, LumenRadianceCache.usf): one workgroup per TRACE
// TILE of a selected probe - an 8x8 of its rays at the tile's level (full, half or a quarter of the probe's
// resolution, RADIANCE_CACHE_TRACE_LEVEL_*), one ray per thread (UE's 8x8 probe trace
// tiles; one group per word of Select's trace tile list, RadianceCacheTraceTileList, a probe's tiles one after the
// other; an indirect dispatch of exactly that many groups, written by Select).
// FPS-RC-TRACE: the tile is a square, not 64 texels in row order - a row of a 32-texel probe spans the whole
// octahedron (sky to ground), so a warp's rays ran as long as its longest march while its sky rays idled; a square's
// rays are neighbours in direction (one warp = 8x4 of them). A probe's tiles are consecutive in the list, so they
// run next to each other on the same origin, object list and field neighbourhood. Every texel keeps its ray:
// the jitter is keyed by the texel's row-order index as before. One workgroup per
// probe looping over its res^2 texels left 100 groups of serial rays on the whole GPU (GPU-TURN-4: 0.62 ms on the
// TURN for 100 probes). The probe was moved out of geometry by the pass before (RadianceCacheSelect.shader, once a
// probe; FPS-RC-TRACE2: every tile of it ran that field walk in its thread 0 behind a barrier) - its ray origin is
// the cell centre plus the probe's offset - and each texel traces one jittered ray by the GI ray contract
// (Common/GITracing.glslh GIRayRadianceFarField: the permutation's far field only - UE's probes trace the global
// SDF, never the mesh SDFs -, the hit lit in place, the sky on a miss) and blends it into the radiance atlas with
// the probe's history. The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none. .a
// keeps the hit distance for the visibility moments.
Shader "RadianceCacheTrace"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/RadianceCacheState.glslh>
        #include <Common/GIFrame.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>

        ReadBuffer(6) GIFrameUB
        {
            GIFrame giFrame;
        };
        ReadBuffer(7) PointLightsUB
        {
            PointLight pointLights[];
        };
        ReadBuffer(8) SpotLightsUB
        {
            SpotLight spotLights[];
        };
        ReadBuffer(9) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(10) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>

        // 32..42: the tracer's own resources (Common/GITracer.glslh, GI_TRACER_BINDING below).
        Uniform sampler LinearClampSampler;
        Uniform(16) textureCube EnvironmentSpecularTexture;
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        Uniform sampler PointClampSampler;
        Uniform(17) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(18) texture2D GBufferATexture;
        #define GBufferATexture sampler2D(GBufferATexture, PointClampSampler)
        Uniform(19) texture2D GBufferEmissiveTexture;
        #define GBufferEmissiveTexture sampler2D(GBufferEmissiveTexture, PointClampSampler)
        // The shading word (uint): a hit's far side and emission mark (GIHitMaterial). 23: the
        // one slot below the surface cache (24..30) and the shared scene-read range (25 up) left by 16..22.
        layout(binding = 23) uniform utexture2D GBufferShadingWordTexture;
        #define GBufferShadingWordTexture usampler2D(GBufferShadingWordTexture, PointClampSampler)
        Uniform(20) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(21) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        layout(binding = 22, rgba16f) restrict uniform image2D RadianceCacheRadianceTexture;

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 24
        // FPS-GI-VOXEL-LIGHT-3: a software probe ray's global-field hit takes its directional shadow from the global
        // field's voxel lighting at 43 (Common/GIHitLighting.glslh declares it on software tracers only;
        // WorldRadianceCache.cpp binds it under the same rule) - one lighting model for every software hit.
        #define GI_VOXEL_LIGHTING_BINDING 43
        #include <Common/GIHitLighting.glslh>

        // How far a probe ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance).
        #define RC_PROBE_TRACE_DISTANCE 20000.0

        // Per texel and frame, a stratified jitter inside the texel (PCG hash).
        vec2 TexelJitter( uint probe, uint texel, uint frame )
        {
            uvec3 v = uvec3( probe, texel, frame ) * 1664525u + 1013904223u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            v ^= v >> 16u;
            v.x += v.y * v.z; v.y += v.z * v.x;
            return vec2( v.xy & 0xFFFFu ) / 65536.0;
        }

        void main()
        {
            // This group's trace tile (RadianceCacheSelect.shader's list; the dispatch's rows of tiles end past it).
            const uint index = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
            if ( index >= radianceCacheSchedule[RC_SCHEDULE_TILES] )
                return;
            const uint               word    = radianceCacheSchedule[RadianceCacheTraceTileList( radianceCache.atlas.z ) + index];
            const uint               probe   = word >> RADIANCE_CACHE_TRACE_TILE_PROBE_SHIFT;
            const uint               tile    = word & RADIANCE_CACHE_TRACE_TILE_MASK;
            const uint               level   = ( word >> RADIANCE_CACHE_TRACE_TILE_LEVEL_SHIFT ) & 3u;
            const uint               skip    = ( word >> RADIANCE_CACHE_TRACE_TILE_SKIP_SHIFT ) & 0xFu;
            const RadianceCacheProbe state   = radianceCacheProbes[probe];
            const uint               clipmap = uint( state.cell.w );
            const vec3               centre  = RadianceCacheWorldCellCentre( radianceCache, state.cell.xyz, clipmap );
            const uint               res     = radianceCache.shape.z;
            // The traced resolution of the tile's level (RADIANCE_CACHE_TRACE_LEVEL_*): one ray per texel, per 2x2
            // or per 4x4 texels.
            const uint traceRes     = RadianceCacheTraceResolution( res, level );
            const uint blockSide    = 1u << level;
            const uint tilesPerSide = RadianceCacheTraceTilesPerSide( res, level );
            // Out of geometry: RadianceCacheSelect.shader wrote this frame's offset to the probe before this pass.
            const vec3 position = centre + state.offset.xyz;

            const ivec2 origin  = RadianceCacheProbeAtlasOrigin( radianceCache, probe, radianceCache.shape.w ) + 1;
            // Never traced, or last traced under the directional lights before the last full update (GRASS-SETTLE):
            // its radiance holds the old sun and is not blended into the new one.
            const bool  fresh   = state.frames.y == 0u || state.frames.y < LightingFrame;
            const float history = fresh ? 0.0 : ( state.frames.w != state.frames.z ? DirtyHistory : HistoryWeight );
            // The cone of one traced texel: its solid angle 4 pi / traceRes^2 as a cone's half angle.
            const float cone    = sqrt( 4.0 / float( traceRes * traceRes ) );
            // This thread's traced texel: its place in the tile's 8x8 (the edge tiles of a traceRes that is no
            // multiple of 8 are cut at the probe's edge). A HALF tile's 4x4 quarter over a square the probe
            // supersamples (skip, RadianceCacheTraceTileWord) is a FULL tile's. No barrier follows, so those
            // threads end.
            const uvec2 square  = uvec2( tile % tilesPerSide, tile / tilesPerSide );
            const ivec2 t       = ivec2( square * 8u + gl_LocalInvocationID.xy );
            const uint  quarter = ( gl_LocalInvocationID.y / 4u ) * 2u + gl_LocalInvocationID.x / 4u;
            if ( t.x < int( traceRes ) && t.y < int( traceRes ) && ( ( skip >> quarter ) & 1u ) == 0u )
            {
                // The texel's row-order index: its jitter sequence (the same ray whatever the tile shape).
                const uint  texel = uint( t.y ) * traceRes + uint( t.x );
                // A full-resolution texel keeps its own jitter sequence; a lower level's ray spans its block.
                const vec2  o = ( vec2( t ) + TexelJitter( probe, texel, FrameIndex ) ) / float( traceRes ) * 2.0 -
                               1.0;
                GITraceInput ray;
                ray.origin    = position;
                ray.tMin      = RadianceCacheProbeTMin( radianceCache, clipmap );
                ray.direction = RadianceCacheOctDecode( o );
                ray.tMax      = RC_PROBE_TRACE_DISTANCE;
                ray.coneAngle = cone;
                ray.flags     = 0u;
                ray.farTMin   = 0.0; // a probe in free space leaves no surface
                float      hitDistance;
                // The far field alone (UE TraceFromProbesCS: the global SDF, no mesh SDFs; GIRayRadianceFarField).
                const vec3 radiance = GIRayRadianceFarField( ray, hitDistance );
                const vec4 traced   = vec4( radiance, hitDistance );
                // The atlas texels the ray stands for: itself, or its 2x2 or 4x4 block (the last row and column of
                // blocks of a res that is no multiple of the block is cut at the probe's edge).
                for ( uint j = 0u; j < blockSide; ++j )
                {
                    for ( uint i = 0u; i < blockSide; ++i )
                    {
                        const ivec2 fine = t * int( blockSide ) + ivec2( i, j );
                        if ( fine.x >= int( res ) || fine.y >= int( res ) )
                            continue;
                        // FPS-GLOW-ROOT: the atlas is a history (an exponential blend over frames), so ONE
                        // non-finite texel - an atlas slot never written since the image was created (its memory
                        // undefined: CreateAtlas, EmptyPixelData) or one NaN ray - stayed NaN for ever:
                        // mix( x, NaN, h ) is NaN for every h > 0, and every screen probe ray that read the probe
                        // through the cache, and every reader of its irradiance, spread it. UE: a history texel
                        // is valid only when written (FRadianceCacheState); here a non-finite last value is no
                        // history (the texel restarts from this ray), and a non-finite ray never enters the atlas
                        // (the texel keeps its last finite value, or 0 on a fresh probe). A finite texel and a
                        // finite ray give exactly the old blend.
                        const bool rayFinite  = !any( isnan( traced ) ) && !any( isinf( traced ) );
                        vec4       value      = rayFinite ? traced : vec4( 0.0, 0.0, 0.0, RC_PROBE_TRACE_DISTANCE );
                        if ( !fresh )
                        {
                            const vec4 last       = imageLoad( RadianceCacheRadianceTexture, origin + fine );
                            const bool lastFinite = !any( isnan( last ) ) && !any( isinf( last ) );
                            if ( lastFinite )
                                value = rayFinite ? mix( traced, last, history ) : last;
                        }
                        imageStore( RadianceCacheRadianceTexture, origin + fine, value );
                    }
                }
            }
        }
    }
}
