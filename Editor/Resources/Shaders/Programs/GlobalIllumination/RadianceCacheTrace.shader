// DesertAsset {"Kind":"Shader","Guid":"a0fa2ae2b4ca49f2940bb34be1953134","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 6/7 — TRACE (UE TraceFromProbes, LumenRadianceCache.usf): one workgroup per TRACE
// TILE of a selected probe - an 8x8 SQUARE of its traced octahedral texels, one ray per thread (UE's 8x8 probe trace
// tiles; workgroup (x = the tile, y = the probe's place in the schedule), WorldRadianceCache's Trace dispatch).
// FPS-RC-TRACE: the tile is a square, not 64 texels in row order - a row of a 32-texel probe spans the whole
// octahedron (sky to ground), so a warp's rays ran as long as its longest march while its sky rays idled; a square's
// rays are neighbours in direction (one warp = 8x4 of them). The probe is the slow axis of the dispatch, so its
// tiles run next to each other on the same origin, object list and field neighbourhood. Every texel keeps its ray:
// the jitter is keyed by the texel's row-order index as before. One workgroup per
// probe looping over its res^2 texels left 100 groups of serial rays on the whole GPU (GPU-TURN-4: 0.62 ms on the
// TURN for 100 probes). The probe was moved out of geometry by the pass before (RadianceCachePlace.shader, once a
// probe; FPS-RC-TRACE2: every tile of it ran that field walk in its thread 0 behind a barrier) - its ray origin is
// the cell centre plus the probe's offset - and each texel traces one jittered ray by the GI ray contract
// (Common/GITracing.glslh: the permutation's tracer, the hit lit in place, the sky on a miss) and blends it into the radiance atlas with the probe's history. The program is compiled per
// tracer (GlobalIllumination::TracerVariant); it names none. .a keeps the hit distance for the visibility moments.
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
        #include <Common/GIHitLighting.glslh>

        // How far a probe ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance).
        #define RC_PROBE_TRACE_DISTANCE 20000.0
        // DOWNSAMPLED FAR PROBES (UE r.Lumen.RadianceCache.DownsampleDistanceFromCamera, GenerateProbeTraceTiles
        // in LumenRadianceCache.usf): a probe whose cell centre is further than this from the camera, cm, traces
        // at half its resolution - one ray per 2x2 texels, jittered over all four, written to each of them with its
        // own history - so it costs a quarter of a near probe's rays. Its converged radiance is the 2x2 box of the
        // full-resolution one; the irradiance and moments (Filter) integrate the same. A probe crossing the
        // distance keeps its atlas texels: only the next traces' rays change.
        #define RC_TRACE_DOWNSAMPLE_DISTANCE 4000.0

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
            const uint count = min( radianceCacheSchedule[RC_SCHEDULE_COUNT], radianceCache.atlas.z );
            if ( gl_WorkGroupID.y >= count )
                return;
            const uint               probe   = radianceCacheSchedule[RC_SCHEDULE_LIST + gl_WorkGroupID.y];
            const RadianceCacheProbe state   = radianceCacheProbes[probe];
            const uint               clipmap = uint( state.cell.w );
            const vec3               centre  = RadianceCacheWorldCellCentre( radianceCache, state.cell.xyz, clipmap );
            const uint               res     = radianceCache.shape.z;
            // The traced resolution: res, or half of it (rounded up) for a far probe; texels per ray side.
            const bool downsampled = distance( giFrame.camera.xyz, centre ) > RC_TRACE_DOWNSAMPLE_DISTANCE;
            const uint traceRes    = downsampled ? ( res + 1u ) / 2u : res;
            const uint blockSide   = downsampled ? 2u : 1u;
            // The dispatch's tiles cover a full-resolution probe (ceil(res / 8)^2 squares); a downsampled one needs
            // the squares of its own traced resolution; the groups past them end.
            const uint tilesPerSide = ( traceRes + 7u ) / 8u;
            const uint tile         = gl_WorkGroupID.x;
            if ( tile >= tilesPerSide * tilesPerSide )
                return;
            // Out of geometry: RadianceCachePlace.shader wrote this frame's offset to the probe before this pass.
            const vec3 position = centre + state.offset.xyz;

            const ivec2 origin  = RadianceCacheProbeAtlasOrigin( radianceCache, probe, radianceCache.shape.w ) + 1;
            // Never traced, or last traced under the directional lights before the last full update (GRASS-SETTLE):
            // its radiance holds the old sun and is not blended into the new one.
            const bool  fresh   = state.frames.y == 0u || state.frames.y < LightingFrame;
            const float history = fresh ? 0.0 : ( state.frames.w != state.frames.z ? DirtyHistory : HistoryWeight );
            // The cone of one traced texel: its solid angle 4 pi / traceRes^2 as a cone's half angle.
            const float cone    = sqrt( 4.0 / float( traceRes * traceRes ) );
            // This thread's traced texel: its place in the tile's 8x8 square (the edge squares of a traceRes that is
            // no multiple of 8 are cut at the probe's edge). No barrier follows, so the threads past them end.
            const uvec2 square = uvec2( tile % tilesPerSide, tile / tilesPerSide );
            const ivec2 t      = ivec2( square * 8u + gl_LocalInvocationID.xy );
            if ( t.x < int( traceRes ) && t.y < int( traceRes ) )
            {
                // The texel's row-order index: its jitter sequence (the same ray whatever the tile shape).
                const uint  texel = uint( t.y ) * traceRes + uint( t.x );
                // A full-resolution texel keeps its own jitter sequence; a downsampled ray's spans its 2x2 block.
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
                const vec3 radiance = GIRayRadiance( ray, hitDistance );
                const vec4 traced   = vec4( radiance, hitDistance );
                // The atlas texels the ray stands for: itself, or its 2x2 block (an odd res's last row and column
                // block is cut at the probe's edge).
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
