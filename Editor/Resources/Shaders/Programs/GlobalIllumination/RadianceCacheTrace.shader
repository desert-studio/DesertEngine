// DesertAsset {"Kind":"Shader","Guid":"a0fa2ae2b4ca49f2940bb34be1953134","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 5/6 — TRACE (UE TraceFromProbes, LumenRadianceCache.usf): one workgroup per selected
// probe. The probe is first moved out of geometry by the tracer (GIMoveOutOfGeometry; not in UE: a world-grid
// probe inside a wall would see only the wall's back faces; DDGI relocates its probes the same way), then each of
// its octahedral texels traces one jittered ray through the GI ray contract (Common/GITracing.glslh: the
// permutation's tracer, the hit lit in place, the sky on a miss) and blends it into the radiance atlas with the
// probe's history. The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none.
// .a keeps the hit distance for the visibility moments.
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

        // 11..15: the tracer's own resources (Common/GITracer.glslh, GI_TRACER_BINDING below).
        Uniform(16) samplerCube u_EnvSpecularTex;
        Uniform(17) sampler2D u_GBufferDepth;
        Uniform(18) sampler2D u_GBufferA;
        Uniform(19) sampler2D u_GBufferEmissive;
        Uniform(20) sampler2D u_RadianceCacheIrradiance;
        Uniform(21) sampler2D u_RadianceCacheDepth;
        layout(binding = 22, rgba16f) restrict uniform image2D u_RadianceCacheRadiance;

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 11
        #include <Common/GITracer.glslh>
        #include <Common/GIHitLighting.glslh>

        // How far a probe ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance).
        #define RC_PROBE_TRACE_DISTANCE 20000.0

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

            if ( gl_LocalInvocationIndex == 0u )
            {
                // Out of geometry: up to a quarter cell of free space around the probe, at most 0.45 cell from the
                // cell centre (the probe stays the cell's).
                const vec3 p = GIMoveOutOfGeometry( centre, 0.25 * cell, 0.45 * cell );
                s_Position   = p;
                radianceCacheProbes[probe].offset = vec4( p - centre, 0.0 );
            }
            barrier();

            const uint  res     = radianceCache.shape.z;
            const ivec2 origin  = RadianceCacheProbeAtlasOrigin( radianceCache, probe, radianceCache.shape.w ) + 1;
            const bool  fresh   = state.frames.y == 0u;
            const float history = fresh ? 0.0 : ( state.frames.w != state.frames.z ? u_DirtyHistory : u_History );
            // The cone of one texel: its solid angle 4 pi / res^2 as a cone's half angle.
            const float cone    = sqrt( 4.0 / float( res * res ) );
            for ( uint texel = gl_LocalInvocationIndex; texel < res * res; texel += 64u )
            {
                const ivec2 t = ivec2( texel % res, texel / res );
                const vec2  o = ( vec2( t ) + TexelJitter( probe, texel, u_FrameIndex ) ) / float( res ) * 2.0 - 1.0;
                GITraceInput ray;
                ray.origin    = s_Position;
                ray.tMin      = RadianceCacheProbeTMin( radianceCache, clipmap );
                ray.direction = RadianceCacheOctDecode( o );
                ray.tMax      = RC_PROBE_TRACE_DISTANCE;
                ray.coneAngle = cone;
                ray.flags     = 0u;
                float      hitDistance;
                const vec3 radiance = GIRayRadiance( ray, hitDistance );
                vec4       value    = vec4( radiance, hitDistance );
                if ( !fresh )
                    value = mix( value, imageLoad( u_RadianceCacheRadiance, origin + t ), history );
                imageStore( u_RadianceCacheRadiance, origin + t, value );
            }
        }
    }
}
