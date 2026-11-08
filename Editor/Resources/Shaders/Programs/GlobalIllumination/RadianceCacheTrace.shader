// DesertAsset {"Kind":"Shader","Guid":"a0fa2ae2b4ca49f2940bb34be1953134","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 5/6 — TRACE (UE TraceFromProbes, LumenRadianceCache.usf): one workgroup per TRACE
// TILE of a selected probe - 64 of its octahedral texels, one ray per thread (UE's probe trace tiles; workgroup
// (x = the probe's place in the schedule, y = the tile), WorldRadianceCache's Trace dispatch). One workgroup per
// probe looping over its res^2 texels left 100 groups of serial rays on the whole GPU (GPU-TURN-4: 0.62 ms on the
// TURN for 100 probes). The probe is first moved out of geometry by the tracer (GIMoveOutOfGeometry; not in UE: a
// world-grid probe inside a wall would see only the wall's back faces; DDGI relocates its probes the same way) -
// by every tile of it, the same deterministic position, written to the probe by each - then each texel traces one
// jittered ray by the GI ray contract (Common/GITracing.glslh GIRayRadianceFarField: the permutation's far field
// only - UE's probes trace the global SDF, never the mesh SDFs -, the hit lit in place, the sky on a miss) and
// blends it into the radiance atlas with the probe's history. The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none. .a keeps the hit distance for the visibility moments.
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

        shared vec3 s_Position;

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
            if ( gl_WorkGroupID.x >= count )
                return;
            const uint               probe   = radianceCacheSchedule[RC_SCHEDULE_LIST + gl_WorkGroupID.x];
            const RadianceCacheProbe state   = radianceCacheProbes[probe];
            const uint               clipmap = uint( state.cell.w );
            const float              cell    = RadianceCacheCellSize( radianceCache, clipmap );
            const vec3               centre  = RadianceCacheWorldCellCentre( radianceCache, state.cell.xyz, clipmap );
            const uint               res     = radianceCache.shape.z;
            // The traced resolution: res, or half of it (rounded up) for a far probe; texels per ray side.
            const bool downsampled = distance( giFrame.camera.xyz, centre ) > RC_TRACE_DOWNSAMPLE_DISTANCE;
            const uint traceRes    = downsampled ? ( res + 1u ) / 2u : res;
            const uint blockSide   = downsampled ? 2u : 1u;
            // The dispatch's tiles cover a full-resolution probe; a downsampled one needs the first quarter. The
            // condition is the group's own (one probe a group), so the rest of the group ends before the barrier.
            if ( gl_WorkGroupID.y * 64u >= traceRes * traceRes )
                return;

            if ( gl_LocalInvocationIndex == 0u )
            {
                // Out of geometry: up to a quarter cell of free space around the probe, at most 0.45 cell from the
                // cell centre (the probe stays the cell's). Each tile of the probe moves it to the same point: the
                // field / scene it reads is this frame's for all of them, and it reads no probe offset.
                const vec3 p = GIMoveOutOfGeometry( centre, 0.25 * cell, 0.45 * cell );
                s_Position   = p;
                radianceCacheProbes[probe].offset = vec4( p - centre, 0.0 );
            }
            barrier();

            const ivec2 origin  = RadianceCacheProbeAtlasOrigin( radianceCache, probe, radianceCache.shape.w ) + 1;
            // Never traced, or last traced under the directional lights before the last full update (GRASS-SETTLE):
            // its radiance holds the old sun and is not blended into the new one.
            const bool  fresh   = state.frames.y == 0u || state.frames.y < LightingFrame;
            const float history = fresh ? 0.0 : ( state.frames.w != state.frames.z ? DirtyHistory : HistoryWeight );
            // The cone of one traced texel: its solid angle 4 pi / traceRes^2 as a cone's half angle.
            const float cone    = sqrt( 4.0 / float( traceRes * traceRes ) );
            // This thread's traced texel: the tile's 64 in row order (the last tile of a traceRes whose square is
            // no multiple of 64 is partly empty). No barrier follows, so the threads past the probe's texels end.
            const uint  texel   = gl_WorkGroupID.y * 64u + gl_LocalInvocationIndex;
            if ( texel < traceRes * traceRes )
            {
                const ivec2 t = ivec2( texel % traceRes, texel / traceRes );
                // A full-resolution texel keeps its own jitter sequence; a downsampled ray's spans its 2x2 block.
                const vec2  o = ( vec2( t ) + TexelJitter( probe, texel, FrameIndex ) ) / float( traceRes ) * 2.0 -
                               1.0;
                GITraceInput ray;
                ray.origin    = s_Position;
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
                // The atlas texels the ray stands for: itself, or its 2x2 block (an odd res's last row and column
                // block is cut at the probe's edge).
                for ( uint j = 0u; j < blockSide; ++j )
                {
                    for ( uint i = 0u; i < blockSide; ++i )
                    {
                        const ivec2 fine = t * int( blockSide ) + ivec2( i, j );
                        if ( fine.x >= int( res ) || fine.y >= int( res ) )
                            continue;
                        vec4 value = traced;
                        if ( !fresh )
                        {
                            const vec4 last = imageLoad( RadianceCacheRadianceTexture, origin + fine );
                            value           = mix( value, last, history );
                        }
                        imageStore( RadianceCacheRadianceTexture, origin + fine, value );
                    }
                }
            }
        }
    }
}
